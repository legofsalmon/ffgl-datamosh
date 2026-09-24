#pragma once

// The licence worker: everything that touches a file or the network, run on a
// background thread, publishing a Gate the render thread reads without locking.
//
// A Service holds no thread of its own. Runtime.cpp owns the one process-wide
// instance and the thread that pumps it; tests construct their own and pump it
// by hand, with a stand-in transport and a temporary folder.

#include "Licence.h"
#include "Store.h"
#include "Wire.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace datamosh::licence {

/// What the platform supplies. Resolved on the worker thread, never on a host
/// call: reading the machine id is registry, IOKit or file I/O.
struct Environment
{
	std::filesystem::path       directory;
	/// The raw platform id — what goes on the wire, and the request code for
	/// offline activation. Empty when it could not be read.
	std::string                 fingerprint;
	std::unique_ptr< Transport > transport;
	/// Opens the licence folder in Finder or Explorer. Optional.
	std::function< bool( const std::filesystem::path& ) > openFolder;
	/// Sent as the activation's `label`, so the account page can tell machines apart.
	std::string                 machineLabel;
};

struct Settings
{
	std::string  product   = PRODUCT;
	std::string  publicKey = PublicKey();
	std::int64_t buildDate = BuildDate();
	Policy       policy    = POLICY;
	std::function< std::int64_t() > now;

	/// The launch check-in waits this long, so it never competes with a
	/// composition loading. Then daily; an unreachable service is retried hourly.
	std::int64_t firstCheckInDelay = 30;
	std::int64_t checkInInterval   = 24 * 60 * 60;
	std::int64_t retryInterval     = 60 * 60;
	/// Re-read the token this often even when nothing changed, so a trial's end
	/// reaches NEW instances without a restart.
	std::int64_t redecideInterval  = 60;
};

class Service
{
public:
	Service( Settings settings, std::function< Environment() > resolveEnvironment );

	Service( const Service& )            = delete;
	Service& operator=( const Service& ) = delete;

	/// Queues what was typed into the Licence field. Any thread; never blocks
	/// on I/O. The worker picks it up on its next pump.
	void Submit( std::string typed );

	/// One round of work: resolve the platform on first call, handle typed
	/// input, import a dropped token, re-read the files if they changed, check
	/// in if it is due. Worker thread only.
	void Pump();

	/// Sleeps until input arrives or `timeout` passes.
	void WaitForWork( std::chrono::milliseconds timeout );

	/// Lock-free: one atomic load. The only call the render thread makes.
	Gate CurrentGate() const;

	/// Bumped whenever the label changes, so an instance can tell whether to
	/// fetch it without taking the lock every frame.
	std::uint32_t Generation() const { return generation.load( std::memory_order_acquire ); }

	/// The Licence field's display name. Takes a short lock; call only when
	/// Generation() moved.
	std::string Label() const;

	// Worker-side state, for tests and the README.
	Status       CurrentStatus() const { return verdict.status; }
	std::string  Notice() const { return notice; }
	const Store* GetStore() const { return store.get(); }
	/// The service's hash of this machine, as a token names it.
	const std::string& MachineHash() const { return localHash; }

private:
	void Resolve();
	void HandleInput( const Input& input );
	void Activate( const std::string& key );
	void StartTrial( const std::string& email );
	void AcceptOffline( const std::string& token, bool fromDropFile );
	void Deactivate();
	void CheckIn( bool askedFor );
	void MaybeCheckIn();
	void ImportDropFile();
	/// Re-reads the token when the files changed, when forced, or when the
	/// last decision is old enough to have been overtaken by the clock.
	void Decide( bool force );
	/// Stores a token only if it verifies, is for this product, and is bound to
	/// this machine. `echoedMachine` is the hash the service says it recorded;
	/// an online reply that disagrees with ours is refused before anything is
	/// written. Offline tokens have no echo and pass `online = false`.
	bool Accept( const std::string& token, bool online, const std::string& echoedMachine,
	             const std::string& key, std::string& why );
	/// Recomputes the label, gate and README from the worker state and
	/// publishes whatever changed.
	void Publish();
	std::string ComposeReadme() const;
	void SetBusy( const std::string& what );
	bool Reachable( const Reply& reply, bool askedFor );

	std::int64_t Now() const;

	Settings                         settings;
	std::function< Environment() >   resolveEnvironment;

	// Worker thread only.
	bool                    resolved = false;
	Environment             environment;
	std::unique_ptr< Store > store;
	std::string             localHash;
	Verdict                 verdict;
	bool                    hasToken        = false;
	/// The service revoked this computer's licence; its words, from the folder.
	std::optional< std::string > revoked;
	bool                    decidedOnce     = false;
	std::string             lastStamp;
	std::int64_t            lastDecision    = 0;
	std::int64_t            startedAt       = 0;
	std::int64_t            nextCheckIn     = 0;
	bool                    launchCheckInDone = false;
	std::string             busy;
	std::string             notice;
	std::string             serviceMessage;   ///< the service's last words, for the README
	std::string             transportProblem; ///< why the service was last unreachable
	std::string             dropHandled;
	std::string             lastReadme;

	// Shared with host threads.
	mutable std::mutex              mutex;
	std::condition_variable         wake;
	std::deque< std::string >       pending;
	std::string                     label = "Licence";
	std::atomic< std::uint32_t >    gate{ 0 };
	std::atomic< std::uint32_t >    generation{ 0 };
};

}  // namespace datamosh::licence
