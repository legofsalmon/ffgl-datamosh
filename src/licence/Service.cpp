#include "Service.h"

#include "CrashMarks.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace datamosh::licence {

namespace {

std::uint32_t Pack( const Gate& gate )
{
	return static_cast< std::uint32_t >( gate.restriction ) |
	       ( static_cast< std::uint32_t >( gate.mark ) << 2 ) | ( gate.decided ? 1u << 4 : 0u );
}

Gate Unpack( std::uint32_t bits )
{
	Gate gate;
	gate.restriction = static_cast< Restriction >( bits & 3u );
	gate.mark        = static_cast< Mark >( ( bits >> 2 ) & 3u );
	gate.decided     = ( bits & ( 1u << 4 ) ) != 0;
	return gate;
}

std::string Date( std::int64_t when )
{
	if( when <= 0 )
		return "-";
	const std::time_t seconds = static_cast< std::time_t >( when );
	std::tm           utc{};
#if defined( _WIN32 )
	gmtime_s( &utc, &seconds );
#else
	gmtime_r( &seconds, &utc );
#endif
	char buffer[ 32 ];
	std::strftime( buffer, sizeof( buffer ), "%Y-%m-%d", &utc );
	return buffer;
}

}  // namespace

Service::Service( Settings settings, std::function< Environment() > resolveEnvironment ) :
	settings( std::move( settings ) ),
	resolveEnvironment( std::move( resolveEnvironment ) )
{
}

std::int64_t Service::Now() const
{
	if( settings.now )
		return settings.now();
	return std::chrono::duration_cast< std::chrono::seconds >(
	           std::chrono::system_clock::now().time_since_epoch() )
	    .count();
}

void Service::Submit( std::string typed )
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		// A host that replays a value, or a restore that writes the field twice,
		// must not queue a pile of identical activations.
		if( pending.empty() || pending.back() != typed )
			pending.push_back( std::move( typed ) );
	}
	wake.notify_one();
}

void Service::ReportCaught( const char* where, const char* what ) noexcept
{
	try
	{
		std::unique_lock< std::mutex > lock( mutex, std::try_to_lock );
		if( !lock.owns_lock() || caught.size() >= 8 )
			return;
		caught.emplace_back( where ? where : "?", what ? what : "?" );
	}
	catch( ... )
	{
		// Out of memory while reporting that something ran out of memory. The
		// log line is already out; this report is not worth a second failure.
	}
}

void Service::WaitForWork( std::chrono::milliseconds timeout )
{
	std::unique_lock< std::mutex > lock( mutex );
	wake.wait_for( lock, timeout, [ this ] { return !pending.empty(); } );
}

Gate Service::CurrentGate() const
{
	return Unpack( gate.load( std::memory_order_acquire ) );
}

std::string Service::Label() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return label;
}

void Service::Resolve()
{
	resolved  = true;
	startedAt = Now();
	nextCheckIn = startedAt + settings.firstCheckInDelay;

	if( resolveEnvironment )
		environment = resolveEnvironment();

	store = std::make_unique< Store >( environment.directory );
	store->Ensure();
	if( !environment.fingerprint.empty() )
		localHash = letissier::machine_hash( environment.fingerprint );

	// Crash reports ride on the same worker, folder and transport. With no
	// folder (the tests' default service) the reporter is inert.
	reports::Reporter::Config config;
	if( !environment.directory.empty() )
	{
		config.folder        = environment.directory / "reports";
		config.installIdFile = environment.directory / "install-id";
	}
	config.binary      = settings.binary;
	config.version     = settings.version;
	config.osVersion   = environment.osVersion;
	config.self        = settings.pid != 0 ? settings.pid : crash::CurrentPid();
	config.alive       = settings.processAlive ? settings.processAlive : crash::ProcessAlive;
	config.now         = [ this ] { return Now(); };
	config.serviceBase = ServiceUrl();
#if defined( _WIN32 )
	const char* home = std::getenv( "USERPROFILE" );
	const char* user = std::getenv( "USERNAME" );
#else
	const char* home = std::getenv( "HOME" );
	const char* user = std::getenv( "USER" );
#endif
	config.home = home ? home : "";
	config.user = user ? user : "";
	reporter    = std::make_unique< reports::Reporter >( std::move( config ), environment.transport.get(),
                                                         environment.openUrl );
	reporter->Start();
}

void Service::Pump()
{
	if( !resolved )
	{
		Resolve();
		Decide( true );
		Publish();
	}

	std::deque< std::string > typed;
	{
		std::lock_guard< std::mutex > lock( mutex );
		typed.swap( pending );
	}
	for( const std::string& text : typed )
	{
		HandleInput( Classify( text ) );
		Publish();
	}

	ImportDropFile();
	Decide( false );
	MaybeCheckIn();

	std::deque< std::pair< std::string, std::string > > problems;
	{
		std::lock_guard< std::mutex > lock( mutex );
		problems.swap( caught );
	}
	for( const auto& problem : problems )
		reporter->Caught( problem.first, problem.second );
	reporter->Pump();

	Publish();
}

void Service::SetBusy( const std::string& what )
{
	busy = what;
	Publish();
}

void Service::HandleInput( const Input& input )
{
	switch( input.kind )
	{
	case InputKind::Empty:
		return;
	case InputKind::Key:
		Activate( input.value );
		return;
	case InputKind::ForeignKey:
		notice = "That key is for " + input.product + ", not Datamosh.";
		return;
	case InputKind::Token:
		AcceptOffline( input.value, false );
		return;
	case InputKind::Email:
		StartTrial( input.value );
		return;
	case InputKind::Folder:
		notice.clear();
		lastReadme.clear();  // rewrite it now, so the folder opens on current text
		Publish();
		if( !environment.openFolder || !environment.openFolder( store->Directory() ) )
			notice = "folder is " + store->Directory().u8string();
		return;
	case InputKind::Deactivate:
		Deactivate();
		return;
	case InputKind::CheckIn:
		CheckIn( true );
		return;
	case InputKind::Feedback:
		notice.clear();
		if( !reporter->OpenFeedback() )
			notice = reporter->Notice();
		return;
	case InputKind::SendReports:
		reporter->Send();
		reporter->Pump();
		notice = reporter->Notice();
		return;
	case InputKind::DiscardReports:
		reporter->Discard();
		notice = reporter->Notice();
		return;
	case InputKind::ReportsOn:
		reporter->SetAutomatic( true );
		reporter->Pump();
		notice = reporter->Notice();
		return;
	case InputKind::ReportsOff:
		reporter->SetAutomatic( false );
		notice = reporter->Notice();
		return;
	case InputKind::Unrecognised:
		notice = "type a key, an email for a trial, or \"folder\"";
		return;
	}
}

bool Service::Reachable( const Reply& reply, bool askedFor )
{
	if( reply.reached )
	{
		transportProblem.clear();
		return true;
	}
	transportProblem = reply.transportError.empty() ? "no response" : reply.transportError;
	if( askedFor )
		notice = "offline - nothing changed, try again when connected";
	return false;
}

void Service::Activate( const std::string& key )
{
	if( environment.fingerprint.empty() )
	{
		notice = "cannot read this computer's id, so it cannot be activated";
		return;
	}
	if( !environment.transport )
	{
		notice = "online activation is not available in this build";
		return;
	}

	SetBusy( "activating..." );
	const Reply reply = ParseReply( environment.transport->Post(
		PATH_ACTIVATE, ActivateBody( key, environment.fingerprint, environment.machineLabel, settings.product ) ) );
	busy.clear();

	if( !Reachable( reply, true ) )
		return;
	if( !reply.ok )
	{
		serviceMessage = reply.message;
		notice         = reply.message;
		return;
	}

	std::string why;
	if( Accept( reply.token, true, reply.machine, key, why ) )
	{
		notice.clear();
		serviceMessage.clear();
		store->RecordCheckIn( Now() );
	}
	else
		notice = why;
	Decide( true );
}

void Service::StartTrial( const std::string& email )
{
	if( environment.fingerprint.empty() )
	{
		notice = "cannot read this computer's id, so a trial cannot start";
		return;
	}
	if( !environment.transport )
	{
		notice = "trials are not available in this build";
		return;
	}

	SetBusy( "starting trial..." );
	const Reply reply = ParseReply( environment.transport->Post(
		PATH_TRIAL, TrialBody( settings.product, email, environment.fingerprint ) ) );
	busy.clear();

	if( !Reachable( reply, true ) )
		return;
	if( !reply.ok )
	{
		serviceMessage = reply.message;
		notice         = reply.message;
		return;
	}

	std::string why;
	if( Accept( reply.token, true, reply.machine, reply.key, why ) )
	{
		notice.clear();
		serviceMessage.clear();
		store->RecordCheckIn( Now() );
	}
	else
		notice = why;
	Decide( true );
}

void Service::AcceptOffline( const std::string& token, bool fromDropFile )
{
	std::string why;
	if( Accept( token, false, std::string(), std::string(), why ) )
	{
		notice.clear();
		if( fromDropFile )
			store->RemoveDropFile();
	}
	else
		notice = fromDropFile ? std::string( Store::DROP_FILE ) + ": " + why : why;
	Decide( true );
}

bool Service::Accept( const std::string& token, bool online, const std::string& echoedMachine,
                      const std::string& key, std::string& why )
{
	if( environment.fingerprint.empty() )
	{
		why = "cannot read this computer's id";
		return false;
	}

	// The service hashes the raw id it is sent and echoes the hash it recorded.
	// If that is not our own hash of the same id, the token it minted is bound
	// to a machine that is not this one, and storing it would turn every later
	// check into "another computer" with no way out by re-activating. Refused
	// here, before anything is written, so the failure is one sentence now
	// rather than a mystery later.
	if( online && echoedMachine != localHash )
	{
		why = "the licence service registered this computer as " +
		      ( echoedMachine.empty() ? std::string( "(nothing)" ) : echoedMachine ) + " but it is " + localHash +
		      "; nothing was stored";
		return false;
	}

	const Verdict checked = licence::Decide( token, environment.fingerprint, settings.buildDate, Now(),
	                                         settings.publicKey, settings.product );
	if( !checked.hasClaims )
	{
		why = online ? "the licence service sent a licence this build cannot verify"
		             : "that is not a Datamosh licence token";
		return false;
	}
	if( checked.status == Status::WrongMachine )
	{
		why = "that token is for another computer - use this computer's request code (README.txt)";
		return false;
	}

	if( !store->WriteToken( token ) )
	{
		why = "could not save the licence in " + store->Directory().u8string();
		return false;
	}
	const std::string keyToKeep = key.empty() ? checked.claims.key : key;
	if( !keyToKeep.empty() )
		store->WriteKey( keyToKeep );
	// A new token from the service outranks any earlier revocation: the
	// licence was reinstated, or this is a different one.
	store->ClearRevoked();
	return true;
}

void Service::Deactivate()
{
	const auto key = store->ReadKey();
	if( key && !environment.fingerprint.empty() && environment.transport )
	{
		// Best effort. Releasing the seat is a courtesy to the account; a
		// machine with no network must still be able to forget its licence.
		SetBusy( "releasing seat..." );
		const Reply reply = ParseReply(
			environment.transport->Post( PATH_DEACTIVATE, DeactivateBody( *key, environment.fingerprint ) ) );
		busy.clear();
		Reachable( reply, false );
		if( reply.reached && !reply.ok )
			serviceMessage = reply.message;
	}
	store->Forget();
	notice = "removed from this computer";
	Decide( true );
}

void Service::CheckIn( bool askedFor )
{
	const auto key = store->ReadKey();
	if( !key || environment.fingerprint.empty() || !environment.transport )
	{
		if( askedFor )
			notice = "no licence key on this computer to check in with";
		return;
	}

	if( askedFor )
		SetBusy( "checking in..." );
	const Reply reply = ParseReply( environment.transport->Post(
		PATH_HEARTBEAT, HeartbeatBody( *key, environment.fingerprint, settings.product ) ) );
	busy.clear();

	if( !Reachable( reply, askedFor ) )
	{
		// The cached token is still the answer. Nothing is restricted by this.
		nextCheckIn = Now() + settings.retryInterval;
		return;
	}

	store->RecordCheckIn( Now() );
	nextCheckIn = Now() + settings.checkInInterval;

	if( !reply.ok )
	{
		serviceMessage = reply.message;
		if( askedFor )
			notice = reply.message;

		// Revoked is the one refusal that ends the licence here: a full refund
		// revokes it, and the refund policy says that ends it. The token goes,
		// so new instances decide as unlicensed; the key stays, so the daily
		// check-in carries on and restores a licence that is reinstated.
		// Running instances keep their latch, so nothing on screen changes.
		//
		// Every other refusal (not activated, expired, a server error) and
		// every network failure keeps the cached token deciding.
		if( reply.reason == "revoked" )
		{
			store->RemoveToken();
			store->WriteRevoked( reply.message.empty() ? std::string( "revoked" ) : reply.message );
			Decide( true );
		}
		return;
	}

	std::string why;
	if( Accept( reply.token, true, reply.machine, *key, why ) )
	{
		serviceMessage.clear();
		if( askedFor )
			notice.clear();
	}
	else
	{
		serviceMessage = why;
		if( askedFor )
			notice = why;
	}
	Decide( true );
}

void Service::MaybeCheckIn()
{
	const std::int64_t now = Now();
	if( now < nextCheckIn )
		return;

	// A revoked licence has no token but keeps checking in with its key, so a
	// reinstatement reaches this computer by itself.
	const bool awaitingReinstatement = !hasToken && revoked.has_value();
	if( ( !hasToken && !awaitingReinstatement ) || verdict.status == Status::Expired || !store->ReadKey() )
	{
		// Nothing to check in with, or a trial that is over and will stay over.
		nextCheckIn = now + settings.retryInterval;
		return;
	}

	// Two plugin binaries share this folder. Whichever checked in last, the
	// other can skip: the launch check-in if either did so in the last ten
	// minutes, the daily one if either did so today.
	const std::int64_t gap  = launchCheckInDone ? settings.checkInInterval : 600;
	const std::int64_t last = store->LastCheckIn();
	launchCheckInDone       = true;
	if( last > 0 && now - last < gap )
	{
		nextCheckIn = last + settings.checkInInterval;
		return;
	}

	CheckIn( false );
}

void Service::ImportDropFile()
{
	const auto dropped = store->ReadDropFile();
	if( !dropped )
	{
		dropHandled.clear();
		return;
	}
	// Tried once per content: a rejected file stays put, with the reason in
	// the README, until someone replaces it.
	if( *dropped == dropHandled )
		return;
	dropHandled = *dropped;

	const Input input = Classify( *dropped );
	if( input.kind != InputKind::Token )
	{
		notice = std::string( Store::DROP_FILE ) + " does not hold a licence token";
		return;
	}
	AcceptOffline( input.value, true );
}

void Service::Decide( bool force )
{
	const std::int64_t now   = Now();
	const std::string  stamp = store->Stamp();
	if( !force && decidedOnce && stamp == lastStamp && now - lastDecision < settings.redecideInterval )
		return;

	// The other binary activated, released or checked in. Whatever this one
	// last had to say about its own attempt no longer describes the licence.
	if( decidedOnce && stamp != lastStamp && !force )
		notice.clear();

	const auto token = store->ReadToken();
	hasToken         = token.has_value();
	revoked          = store->ReadRevoked();
	verdict          = token ? licence::Decide( *token, environment.fingerprint, settings.buildDate, now,
	                                            settings.publicKey, settings.product )
	                         : Verdict{};
	lastStamp    = stamp;
	lastDecision = now;
	decidedOnce  = true;
}

void Service::Publish()
{
	const bool keyConfigured = PublicKeyConfigured( settings.publicKey );
	const bool identified    = !environment.fingerprint.empty();

	Gate next;
	next.decided = decidedOnce;
	if( decidedOnce && identified )
	{
		next.restriction = RestrictionFor( verdict.status, settings.policy, keyConfigured );
		next.mark        = next.restriction == Restriction::None ? Mark::None : MarkFor( verdict.status );
	}
	// A machine whose id cannot be read cannot be licensed, and must not be
	// punished for it either: it renders unrestricted and says why.

	std::string text;
	if( !busy.empty() )
		text = busy;
	else
	{
		if( !notice.empty() )
			text = notice;
		else if( !identified && decidedOnce )
			text = "cannot read this computer's id";
		else if( !decidedOnce )
			text = "checking...";
		else if( !hasToken && revoked )
			text = "revoked";
		else
			text = DescribeStatus( verdict.status, hasToken, verdict.hasClaims ? &verdict.claims : nullptr,
			                       Now(), keyConfigured );
		// A locked instance renders an untouched passthrough, which is also
		// what a plugin that never loaded renders. The name is where the
		// difference shows, so it says so first — including while it shows
		// the answer to something typed, which stays up until the next input.
		if( next.restriction == Restriction::Lock )
			text = "locked, " + text;

		// The one-time question after a problem. A plugin has no dialog to
		// ask it in; the field's name is the only thing it can put in front
		// of someone, and it is where they answer. It goes when they do, or
		// when Resolume next starts — asked once, and unanswered is "no".
		if( reporter && reporter->Asking() )
			text += reporter->AskingAboutACrash() ? " | closed unexpectedly last time - type send or discard"
			                                      : " | a problem was caught - type send or discard";
	}
	const std::string nextLabel = "Licence: " + text;

	bool changed = false;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( nextLabel != label )
		{
			label   = nextLabel;
			changed = true;
		}
	}
	const std::uint32_t packed = Pack( next );
	if( gate.exchange( packed, std::memory_order_acq_rel ) != packed )
		changed = true;
	if( changed )
		generation.fetch_add( 1, std::memory_order_acq_rel );

	if( store && decidedOnce )
	{
		const std::string readme = ComposeReadme();
		if( readme != lastReadme && store->WriteReadme( readme ) )
			lastReadme = readme;
	}
}

std::string Service::ComposeReadme() const
{
	std::ostringstream out;
	out << "Datamosh licence\n"
	    << "================\n\n"
	    << "This folder holds the Datamosh licence for this computer. It is shared by\n"
	    << "the Datamosh effect and the Mosh Transplant blend mode, and nothing in it\n"
	    << "is ever saved into a Resolume composition.\n\n";

	out << "Status: " << Label().substr( 9 ) << "\n";
	if( verdict.hasClaims )
	{
		const Claims& claims = verdict.claims;
		out << "Licence key: " << claims.key << "\n";
		if( !claims.name.empty() )
			out << "Licensed to: " << claims.name << "\n";
		out << "Edition: " << claims.edition << ", " << claims.seats << ( claims.seats == 1 ? " seat" : " seats" )
		    << "\n";
		if( claims.edition == "trial" )
			out << "Trial ends: " << Date( claims.exp ) << "\n";
		else
		{
			out << "Updates included until: " << Date( claims.maint_until ) << "\n";
			out << "Next check-in due by: " << Date( claims.exp )
			    << " (it happens by itself when this computer is online)\n";
		}
	}
	if( !hasToken && revoked )
		out << "\nThis licence was revoked by the licence service: " << *revoked << "\n"
		    << "The licence key is kept on this computer. If the licence is reinstated, the\n"
		    << "next check-in restores it by itself; type \"check\" into the Licence field\n"
		    << "to check straight away.\n\n";
	out << "This build: " << DATAMOSH_VERSION << ", built " << Date( settings.buildDate ) << "\n";
	if( !serviceMessage.empty() )
		out << "\nLast word from the licence service: " << serviceMessage << "\n";
	if( !transportProblem.empty() )
		out << "\nThe licence service was last unreachable: " << transportProblem
		    << "\n(That never restricts anything; the saved licence keeps working.)\n";

	if( reporter )
		out << "\n" << reporter->Describe();

	out << "\nRequest code (for offline activation):\n"
	    << "    " << ( environment.fingerprint.empty() ? "(this computer's id could not be read)"
	                                                   : environment.fingerprint )
	    << "\n";
	if( !localHash.empty() )
		out << "The licence service names this computer: " << localHash << "\n";

	out << "\nHow to license Datamosh\n"
	    << "-----------------------\n\n"
	    << "Everything is typed into the Licence field, at the bottom of the Datamosh\n"
	    << "effect's parameters (or Mosh Transplant's). Press Enter; the field empties\n"
	    << "and its name shows what happened.\n\n"
	    << "  Your licence key    LT-DATA-XXXX-XXXX-XXXX   activates this computer\n"
	    << "  Your email address                           starts a free trial\n"
	    << "  folder                                       opens this folder\n"
	    << "  check                                        checks in now\n"
	    << "  deactivate                                   releases this computer's seat\n\n"
	    << "Offline: on any computer with internet, sign in at " << ACCOUNT_URL << ",\n"
	    << "choose the licence, enter the request code above, and save the token it\n"
	    << "gives you in this folder as a file called " << Store::DROP_FILE << ".\n"
	    << "Datamosh picks it up within a few seconds and removes the file. (Pasting\n"
	    << "the token into the Licence field works too, if Resolume accepts it.)\n\n";

	switch( settings.policy )
	{
	case Policy::Open:
		out << "An unlicensed copy of this build works fully and is not marked.\n";
		break;
	case Policy::Watermark:
		out << "An unlicensed copy works fully but marks its output. Licensing removes the\n"
		    << "mark straight away. A licence that lapses never marks an effect that is\n"
		    << "already running; only instances added after it.\n";
		break;
	case Policy::Lock:
		out << "Without a licence or a running trial, Datamosh is locked: it passes video\n"
		    << "through untouched, and the Licence field's name says \"locked\". Licensing\n"
		    << "or starting a trial unlocks it straight away, including effects already in\n"
		    << "a composition. A licence or trial that ends never locks an effect that is\n"
		    << "already running; only effects added after it, or after Resolume restarts.\n";
		break;
	}
	return out.str();
}

}  // namespace datamosh::licence
