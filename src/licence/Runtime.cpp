#include "Runtime.h"

#include "CrashMarks.h"
#include "Platform.h"
#include "Service.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <system_error>
#include <thread>

namespace datamosh::licence {

namespace {

std::atomic< Service* > installed{ nullptr };
std::atomic< Service* > running{ nullptr };
std::once_flag          started;

/// Set once, by the first NameBinary, before the worker starts.
std::atomic< const char* > binaryName{ "Datamosh" };
std::mutex                 reportsFolderMutex;
std::filesystem::path      testReportsFolder;
std::mutex                 hostMutex;
std::string                hostName;
std::string                hostVersion;

/// Creates and maps this process's crash marker. On the worker thread, or
/// in a test; never on a host call.
void OpenMarkerIn( const std::filesystem::path& reportsFolder )
{
	if( reportsFolder.empty() )
		return;
	if( !crash::Open( reportsFolder, binaryName.load( std::memory_order_acquire ), DATAMOSH_VERSION ) )
		return;
	std::lock_guard< std::mutex > lock( hostMutex );
	if( !hostName.empty() || !hostVersion.empty() )
		crash::SetHost( hostName.c_str(), hostVersion.c_str() );
}

Service* Active()
{
	if( Service* service = installed.load( std::memory_order_acquire ) )
		return service;
	return running.load( std::memory_order_acquire );
}

void StartWorker()
{
	// Deliberately leaked, and the thread deliberately never joined.
	//
	// Joining would have to happen when the host unloads the plugin, and on
	// Windows that is DllMain under the loader lock, where waiting for a thread
	// to exit deadlocks. Stopping it from an instance's destructor instead would
	// block the host's thread for as long as an HTTP request takes to time out,
	// which is exactly the stall a live effect must never cause. So the module
	// is pinned (it cannot be unmapped under the thread) and the Service is
	// never destroyed (the thread can never touch a destroyed object, even
	// while static destructors run at exit). The process ending is what stops
	// it.
	Settings settings;
	settings.binary = binaryName.load( std::memory_order_acquire );
	auto* service   = new Service( std::move( settings ), [] { return PlatformEnvironment(); } );

	try
	{
		PinThisModule();
		std::thread( [ service ] {
			// The marker before anything else, so an instance can claim its
			// slot within a frame or two of the first one.
			try
			{
				const std::filesystem::path base = PlatformDirectory();
				OpenMarkerIn( base.empty() ? base : base / "reports" );
			}
			catch( ... )
			{
				// No marker: instances run without a breadcrumb.
			}
			for( ;; )
			{
				service->Pump();
				service->WaitForWork( std::chrono::seconds( 5 ) );
			}
		} ).detach();
	}
	catch( const std::system_error& )
	{
		// No thread, no licence checking: unrestricted. Never the other way.
		return;
	}

	running.store( service, std::memory_order_release );
}

}  // namespace

void Start()
{
	if( installed.load( std::memory_order_acquire ) )
		return;
	std::call_once( started, StartWorker );
}

Gate CurrentGate()
{
	Service* service = Active();
	return service ? service->CurrentGate() : Gate{};
}

std::uint32_t LabelGeneration()
{
	Service* service = Active();
	return service ? service->Generation() : 0u;
}

std::string CurrentLabel()
{
	Service* service = Active();
	return service ? service->Label() : std::string( "Licence" );
}

void Submit( const std::string& typed )
{
	Start();
	if( Service* service = Active() )
		service->Submit( typed );
}

void NameBinary( const char* binary )
{
	static std::once_flag named;
	std::call_once( named, [ binary ] {
		if( binary )
			binaryName.store( binary, std::memory_order_release );
	} );
}

bool CrashMarkerOpen()
{
	return crash::IsOpen();
}

BreadcrumbSlot* ClaimBreadcrumb()
{
	return crash::Claim();
}

void ReleaseBreadcrumb( BreadcrumbSlot* slot )
{
	crash::Release( slot );
}

void NoteHost( const char* name, const char* version )
{
	std::lock_guard< std::mutex > lock( hostMutex );
	hostName    = name ? name : "";
	hostVersion = version ? version : "";
	crash::SetHost( hostName.c_str(), hostVersion.c_str() );
}

void ReportCaught( const char* where, const char* what ) noexcept
{
	if( Service* service = Active() )
		service->ReportCaught( where, what );
}

void OpenFeedback()
{
	Submit( "feedback" );
}

namespace testing {

void Install( Service* service )
{
	installed.store( service, std::memory_order_release );
}

void SetReportsFolder( const std::filesystem::path& folder )
{
	std::lock_guard< std::mutex > lock( reportsFolderMutex );
	testReportsFolder = folder;
}

void OpenCrashMarker()
{
	std::filesystem::path folder;
	{
		std::lock_guard< std::mutex > lock( reportsFolderMutex );
		folder = testReportsFolder;
	}
	OpenMarkerIn( folder );
}

}  // namespace testing

}  // namespace datamosh::licence
