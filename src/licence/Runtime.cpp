#include "Runtime.h"

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
	auto* service = new Service( Settings{}, [] { return PlatformEnvironment(); } );

	try
	{
		PinThisModule();
		std::thread( [ service ] {
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

namespace testing {

void Install( Service* service )
{
	installed.store( service, std::memory_order_release );
}

}  // namespace testing

}  // namespace datamosh::licence
