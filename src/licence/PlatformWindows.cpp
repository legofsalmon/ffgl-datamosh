// Windows: MachineGuid from the registry, %APPDATA%, and WinHTTP.
//
// WinHTTP rather than anything bundled: it is part of every Windows Resolume
// runs on, it uses the system's certificate store and proxy settings, and it
// adds nothing a customer has to install.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#include <winhttp.h>

#include "Platform.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace datamosh::licence {

namespace {

std::wstring Wide( const std::string& utf8 )
{
	if( utf8.empty() )
		return {};
	const int length = MultiByteToWideChar( CP_UTF8, 0, utf8.data(), static_cast< int >( utf8.size() ), nullptr, 0 );
	std::wstring out( static_cast< size_t >( length ), L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, utf8.data(), static_cast< int >( utf8.size() ), out.data(), length );
	return out;
}

std::string Utf8( const std::wstring& wide )
{
	if( wide.empty() )
		return {};
	const int length = WideCharToMultiByte( CP_UTF8, 0, wide.data(), static_cast< int >( wide.size() ), nullptr, 0,
	                                        nullptr, nullptr );
	std::string out( static_cast< size_t >( length ), '\0' );
	WideCharToMultiByte( CP_UTF8, 0, wide.data(), static_cast< int >( wide.size() ), out.data(), length, nullptr,
	                     nullptr );
	return out;
}

/// Closes a WinHTTP handle on scope exit.
struct Handle
{
	HINTERNET value = nullptr;
	explicit Handle( HINTERNET handle ) :
		value( handle )
	{
	}
	~Handle()
	{
		if( value )
			WinHttpCloseHandle( value );
	}
	Handle( const Handle& )            = delete;
	Handle& operator=( const Handle& ) = delete;
	explicit operator bool() const { return value != nullptr; }
};

std::string LastError( const char* what )
{
	return std::string( what ) + " failed (error " + std::to_string( GetLastError() ) + ")";
}

class WinHttpTransport : public Transport
{
public:
	HttpResponse Post( const std::string& path, const std::string& jsonBody ) override
	{
		return Post( path, jsonBody, PostOptions{} );
	}

	HttpResponse Post( const std::string& path, const std::string& jsonBody, const PostOptions& options ) override
	{
		HttpResponse response;

		const ServiceAddress address = ParseServiceUrl( ServiceUrl() );
		if( !address.valid )
		{
			response.error = "bad service URL";
			return response;
		}
		const std::wstring agent =
			options.userAgent.empty() ? std::wstring( L"Datamosh/" DATAMOSH_VERSION ) : Wide( options.userAgent );

		// Automatic proxy discovery exists from Windows 8.1; before that, the
		// configured default proxy is the best available.
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
		const DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#else
		const DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#endif
		Handle session( WinHttpOpen( agent.c_str(), access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 ) );
		if( !session )
		{
			response.error = LastError( "WinHttpOpen" );
			return response;
		}
		// Resolve, connect, send, receive: all bounded, so a dead network costs
		// the worker thread fifteen seconds and nothing else. WinHTTP bounds
		// each phase rather than the whole, so a caller's budget is split
		// across the four and their sum is the budget.
		if( options.timeoutSeconds > 0 )
		{
			const int total = options.timeoutSeconds * 1000;
			WinHttpSetTimeouts( session.value, total / 8, total / 8, total / 4, total / 2 );
		}
		else
			WinHttpSetTimeouts( session.value, 10000, 10000, 15000, 15000 );

		const std::wstring host = Wide( address.host );
		Handle connection( WinHttpConnect( session.value, host.c_str(), static_cast< INTERNET_PORT >( address.port ), 0 ) );
		if( !connection )
		{
			response.error = LastError( "WinHttpConnect" );
			return response;
		}

		const std::wstring widePath = Wide( address.basePath + path );
		Handle             request( WinHttpOpenRequest( connection.value, L"POST", widePath.c_str(), nullptr,
		                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		                                                address.secure ? WINHTTP_FLAG_SECURE : 0 ) );
		if( !request )
		{
			response.error = LastError( "WinHttpOpenRequest" );
			return response;
		}

		const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
		if( !WinHttpSendRequest( request.value, headers, static_cast< DWORD >( -1L ),
		                         const_cast< char* >( jsonBody.data() ), static_cast< DWORD >( jsonBody.size() ),
		                         static_cast< DWORD >( jsonBody.size() ), 0 ) )
		{
			response.error = LastError( "WinHttpSendRequest" );
			return response;
		}
		if( !WinHttpReceiveResponse( request.value, nullptr ) )
		{
			response.error = LastError( "WinHttpReceiveResponse" );
			return response;
		}

		DWORD status = 0;
		DWORD size   = sizeof( status );
		if( !WinHttpQueryHeaders( request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX ) )
		{
			response.error = LastError( "WinHttpQueryHeaders" );
			return response;
		}

		std::string body;
		for( ;; )
		{
			DWORD available = 0;
			if( !WinHttpQueryDataAvailable( request.value, &available ) )
			{
				response.error = LastError( "WinHttpQueryDataAvailable" );
				return response;
			}
			if( available == 0 )
				break;
			// A licence reply is a few hundred bytes. Anything enormous is not
			// the service, and is not worth holding.
			if( body.size() + available > 1024 * 1024 )
			{
				response.error = "reply too large";
				return response;
			}
			std::vector< char > chunk( available );
			DWORD               read = 0;
			if( !WinHttpReadData( request.value, chunk.data(), available, &read ) )
			{
				response.error = LastError( "WinHttpReadData" );
				return response;
			}
			body.append( chunk.data(), read );
		}

		response.reached = true;
		response.status  = static_cast< int >( status );
		response.body    = std::move( body );
		return response;
	}
};

std::string MachineGuid()
{
	wchar_t value[ 128 ] = {};
	DWORD   size         = sizeof( value );
	// The 64-bit view explicitly. A 32-bit process would otherwise be
	// redirected to a Wow6432Node copy that does not hold MachineGuid.
	const LSTATUS result =
		RegGetValueW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
		              RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &size );
	if( result != ERROR_SUCCESS )
		return {};
	return Utf8( value );
}

std::filesystem::path Directory()
{
	PWSTR roaming = nullptr;
	std::filesystem::path base;
	if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming ) ) && roaming )
		base = roaming;
	if( roaming )
		CoTaskMemFree( roaming );
	if( base.empty() )
	{
		if( const wchar_t* appData = _wgetenv( L"APPDATA" ); appData && *appData )
			base = appData;
		else
			return {};
	}
	return base / L"LeTissier" / L"Datamosh";
}

bool Explore( const std::wstring& target )
{
	// Explorer as a process rather than ShellExecute, which wants COM
	// initialised on the calling thread — this is a worker thread in a host's
	// process, and COM apartment state is not ours to set there. Handed a
	// folder it opens it; handed an http(s) URL it opens the default browser.
	std::wstring command = L"explorer.exe \"" + target + L"\"";
	STARTUPINFOW         startup{};
	startup.cb = sizeof( startup );
	PROCESS_INFORMATION process{};
	if( !CreateProcessW( nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
	                     &process ) )
		return false;
	CloseHandle( process.hThread );
	CloseHandle( process.hProcess );
	return true;
}

bool OpenFolder( const std::filesystem::path& folder )
{
	return Explore( folder.wstring() );
}

bool OpenUrl( const std::string& url )
{
	// Only ever the service's own feedback page. Nothing with a quote in it,
	// which is all it would take to add an argument to the command line.
	if( ( url.rfind( "https://", 0 ) != 0 && url.rfind( "http://", 0 ) != 0 ) ||
	    url.find_first_of( "\"\r\n" ) != std::string::npos )
		return false;
	return Explore( Wide( url ) );
}

std::string OsVersion()
{
	// RtlGetVersion rather than GetVersionEx, which reports 6.2 to any
	// process without a manifest saying otherwise — and that is the host's
	// manifest, not ours.
	using RtlGetVersionFunction = LONG( WINAPI* )( PRTL_OSVERSIONINFOW );
	HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
	if( !ntdll )
		return {};
	auto getVersion = reinterpret_cast< RtlGetVersionFunction >( GetProcAddress( ntdll, "RtlGetVersion" ) );
	if( !getVersion )
		return {};
	RTL_OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof( info );
	if( getVersion( &info ) != 0 )
		return {};
	return std::to_string( info.dwMajorVersion ) + "." + std::to_string( info.dwMinorVersion ) + "." +
	       std::to_string( info.dwBuildNumber );
}

std::string ComputerName()
{
	wchar_t name[ MAX_COMPUTERNAME_LENGTH + 1 ] = {};
	DWORD   size                                = MAX_COMPUTERNAME_LENGTH + 1;
	if( !GetComputerNameW( name, &size ) )
		return {};
	return Utf8( std::wstring( name, size ) );
}

}  // namespace

std::filesystem::path PlatformDirectory()
{
	return Directory();
}

std::unique_ptr< Transport > MakePlatformTransport()
{
	return std::make_unique< WinHttpTransport >();
}

Environment PlatformEnvironment()
{
	Environment environment;
	environment.directory    = Directory();
	environment.fingerprint  = MachineGuid();
	environment.transport    = MakePlatformTransport();
	environment.openFolder   = OpenFolder;
	environment.openUrl      = OpenUrl;
	environment.osVersion    = OsVersion();
	environment.machineLabel = ComputerName();
	return environment;
}

void PinThisModule()
{
	HMODULE module = nullptr;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                    reinterpret_cast< LPCWSTR >( &PinThisModule ), &module );
}

}  // namespace datamosh::licence
