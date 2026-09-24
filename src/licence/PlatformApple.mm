// macOS: IOPlatformUUID from IOKit, ~/Library/Application Support, and
// NSURLSession.
//
// Compiled with ARC. Nothing here runs on a host thread: the worker calls
// PlatformEnvironment once, and the transport only ever from the worker.

#include "Platform.h"

#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>

#include <dlfcn.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace datamosh::licence {

namespace {

class UrlSessionTransport : public Transport
{
public:
	HttpResponse Post( const std::string& path, const std::string& jsonBody ) override
	{
		return Post( path, jsonBody, PostOptions{} );
	}

	HttpResponse Post( const std::string& path, const std::string& jsonBody, const PostOptions& options ) override
	{
		@autoreleasepool
		{
			HttpResponse response;

			// The licence calls keep the long default; a report asks for 8 s,
			// and that is the whole request, not each phase of it.
			const double timeout = options.timeoutSeconds > 0 ? options.timeoutSeconds : 15.0;
			const double resource = options.timeoutSeconds > 0 ? options.timeoutSeconds : 20.0;
			const double wait     = options.timeoutSeconds > 0 ? options.timeoutSeconds + 2.0 : 25.0;

			const std::string base = ServiceUrl();
			NSString* address = [NSString stringWithFormat:@"%s%s", base.c_str(), path.c_str()];
			NSURL*    url     = [NSURL URLWithString:address];
			if( url == nil )
			{
				response.error = "bad URL";
				return response;
			}

			NSMutableURLRequest* request =
				[NSMutableURLRequest requestWithURL:url
				                        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
				                    timeoutInterval:timeout];
			request.HTTPMethod = @"POST";
			[request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
			[request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
			if( !options.userAgent.empty() )
				[request setValue:[NSString stringWithUTF8String:options.userAgent.c_str()]
				    forHTTPHeaderField:@"User-Agent"];
			request.HTTPBody = [NSData dataWithBytes:jsonBody.data() length:jsonBody.size()];

			// Ephemeral: no cookies, no cache, nothing written to disk on the
			// host application's behalf.
			NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
			configuration.timeoutIntervalForRequest  = timeout;
			configuration.timeoutIntervalForResource = resource;
			NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];

			// __block storage lives on the heap once the block is copied, so the
			// completion handler can still write here safely if the wait below
			// gives up first and this frame is gone.
			__block NSData*          received   = nil;
			__block NSInteger        statusCode = 0;
			__block NSString*        failure    = nil;
			dispatch_semaphore_t     done       = dispatch_semaphore_create( 0 );

			NSURLSessionDataTask* task =
				[session dataTaskWithRequest:request
				           completionHandler:^( NSData* data, NSURLResponse* reply, NSError* error ) {
					           if( error != nil )
						           failure = error.localizedDescription;
					           else
					           {
						           received = data;
						           if( [reply isKindOfClass:[NSHTTPURLResponse class]] )
							           statusCode = ( (NSHTTPURLResponse*)reply ).statusCode;
					           }
					           dispatch_semaphore_signal( done );
				           }];
			[task resume];

			const long timedOut =
				dispatch_semaphore_wait( done, dispatch_time( DISPATCH_TIME_NOW, static_cast< int64_t >( wait * NSEC_PER_SEC ) ) );
			[session finishTasksAndInvalidate];

			if( timedOut != 0 )
			{
				[task cancel];
				response.error = "timed out";
				return response;
			}
			if( failure != nil )
			{
				response.error = failure.UTF8String ? failure.UTF8String : "request failed";
				return response;
			}
			if( statusCode == 0 )
			{
				response.error = "no HTTP response";
				return response;
			}

			response.reached = true;
			response.status  = static_cast< int >( statusCode );
			if( received != nil )
				response.body.assign( static_cast< const char* >( received.bytes ), received.length );
			return response;
		}
	}
};

std::string PlatformUuid()
{
	// 0 is the default main port. kIOMainPortDefault is macOS 12 and later and
	// kIOMasterPortDefault is deprecated there; the literal works on both.
	io_service_t expert = IOServiceGetMatchingService( 0, IOServiceMatching( "IOPlatformExpertDevice" ) );
	if( expert == 0 )
		return {};

	std::string  uuid;
	CFTypeRef    value = IORegistryEntryCreateCFProperty( expert, CFSTR( kIOPlatformUUIDKey ), kCFAllocatorDefault, 0 );
	if( value != nullptr )
	{
		if( CFGetTypeID( value ) == CFStringGetTypeID() )
		{
			char buffer[ 128 ] = {};
			if( CFStringGetCString( static_cast< CFStringRef >( value ), buffer, sizeof( buffer ),
			                        kCFStringEncodingUTF8 ) )
				uuid = buffer;
		}
		CFRelease( value );
	}
	IOObjectRelease( expert );
	return uuid;
}

std::filesystem::path Directory()
{
	@autoreleasepool
	{
		NSArray< NSString* >* found =
			NSSearchPathForDirectoriesInDomains( NSApplicationSupportDirectory, NSUserDomainMask, YES );
		NSString* base = found.firstObject;
		if( base == nil || base.fileSystemRepresentation == nullptr )
			return {};
		return std::filesystem::path( base.fileSystemRepresentation ) / "LeTissier" / "Datamosh";
	}
}

bool Open( const std::string& target )
{
	// /usr/bin/open rather than NSWorkspace, whose thread-safety from a
	// background thread inside someone else's application is not ours to lean
	// on. The same command opens a folder in Finder and a URL in the default
	// browser.
	char              open[] = "/usr/bin/open";
	char*             argv[] = { open, const_cast< char* >( target.c_str() ), nullptr };
	pid_t             child  = 0;
	if( posix_spawn( &child, open, nullptr, nullptr, argv, environ ) != 0 )
		return false;
	int status = 0;
	waitpid( child, &status, 0 );
	return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
}

bool OpenFolder( const std::filesystem::path& folder )
{
	return Open( folder.string() );
}

bool OpenUrl( const std::string& url )
{
	// Only ever the service's own feedback page; refusing anything else keeps
	// this from being a way to launch arbitrary things from a text field.
	if( url.rfind( "https://", 0 ) != 0 && url.rfind( "http://", 0 ) != 0 )
		return false;
	return Open( url );
}

std::string OsVersion()
{
	char   buffer[ 64 ] = {};
	size_t size         = sizeof( buffer ) - 1;
	if( sysctlbyname( "kern.osproductversion", buffer, &size, nullptr, 0 ) != 0 )
		return {};
	return buffer;
}

std::string HostName()
{
	char host[ 256 ] = {};
	if( gethostname( host, sizeof( host ) - 1 ) != 0 )
		return {};
	std::string name = host;
	// "Studio-MacBook.local" reads better on the account page without ".local".
	const size_t local = name.rfind( ".local" );
	if( local != std::string::npos && local + 6 == name.size() )
		name.erase( local );
	return name;
}

}  // namespace

std::filesystem::path PlatformDirectory()
{
	return Directory();
}

std::unique_ptr< Transport > MakePlatformTransport()
{
	return std::make_unique< UrlSessionTransport >();
}

Environment PlatformEnvironment()
{
	Environment environment;
	environment.directory    = Directory();
	environment.fingerprint  = PlatformUuid();
	environment.transport    = MakePlatformTransport();
	environment.openFolder   = OpenFolder;
	environment.openUrl      = OpenUrl;
	environment.osVersion    = OsVersion();
	environment.machineLabel = HostName();
	return environment;
}

void PinThisModule()
{
	Dl_info info{};
	if( dladdr( reinterpret_cast< void* >( &PinThisModule ), &info ) != 0 && info.dli_fname )
		dlopen( info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE );
}

}  // namespace datamosh::licence
