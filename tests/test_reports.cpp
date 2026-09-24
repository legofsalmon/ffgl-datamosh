// Crash reports and feedback: the scrubber, the payload, the queue, the
// consent rules, and the marker that lets the next load see that the last one
// died mid-frame.
//
// Everything that could leave this computer is pinned here. The rule that
// matters most — nothing is sent without the setting or a "send" — has its own
// test, and it runs the worker for simulated hours to prove it.

#include "harness/Licence.h"
#include "harness/Synthetic.h"
#include "harness/TestRunner.h"

#include <CrashMarks.h>
#include <DatamoshEffect.h>
#include <DatamoshMixer.h>
#include <Json.h>
#include <Reports.h>
#include <Runtime.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if !defined( _WIN32 )
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace datamosh::test {

namespace {

namespace fs      = std::filesystem;
namespace reports = licence::reports;
namespace crash   = licence::crash;

/// A process id that is not this one; the rig says every such process is gone.
constexpr std::uint32_t DEAD_PID = 4'000'001;

std::string Slurp( const fs::path& path )
{
	std::ifstream      file( path, std::ios::binary );
	std::ostringstream contents;
	contents << file.rdbuf();
	return contents.str();
}

size_t JsonFilesIn( const fs::path& folder )
{
	size_t          count = 0;
	std::error_code error;
	for( fs::directory_iterator it( folder, error ), end; !error && it != end; it.increment( error ) )
		count += it->path().extension() == ".json" ? 1 : 0;
	return count;
}

/// Leaves a marker behind exactly as a process that died mid-frame would.
void PlantDeadMarker( const fs::path& licenceFolder, std::uint32_t pid, const char* stage, const char* binary = "Datamosh" )
{
	crash::MarkFile marker{};
	std::memcpy( marker.magic, crash::MAGIC, sizeof( crash::MAGIC ) );
	marker.layout = crash::LAYOUT;
	marker.pid    = pid;
	std::strncpy( marker.binary, binary, sizeof( marker.binary ) - 1 );
	std::strncpy( marker.version, "1.0.0", sizeof( marker.version ) - 1 );
	std::strncpy( marker.host, "Resolume Arena 7.22.1", sizeof( marker.host ) - 1 );
	marker.slots[ 0 ].claimed = 1;
	marker.slots[ 0 ].inFrame = 0;  // a second instance, idle when it died
	marker.slots[ 2 ].claimed = 1;
	marker.slots[ 2 ].inFrame = 1;
	marker.slots[ 2 ].frames  = 1234;
	marker.slots[ 2 ].width   = 1920;
	marker.slots[ 2 ].height  = 1080;
	std::strncpy( marker.slots[ 2 ].stage, stage, sizeof( marker.slots[ 2 ].stage ) - 1 );

	const fs::path folder = crash::RunningFolder( licenceFolder / "reports" );
	fs::create_directories( folder );
	std::ofstream file( folder / ( std::string( binary ) + "-" + std::to_string( pid ) + ".mark" ), std::ios::binary );
	file.write( reinterpret_cast< const char* >( &marker ), sizeof( marker ) );
}

/// The fields a crash report may carry, and nothing else.
bool OnlyContractFields( const std::map< std::string, licence::json::Value >& fields )
{
	static const std::set< std::string > allowed = { "product", "version", "os",   "osVersion", "arch",
	                                                 "install", "kind",    "summary", "detail",  "signature",
	                                                 "occurredAt" };
	for( const auto& field : fields )
		if( allowed.count( field.first ) == 0 )
			return false;
	return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// On the device, before anything is written
// ---------------------------------------------------------------------------

TEST( TheScrubberRemovesHomeFoldersUserNamesAndQueryStrings )
{
	const std::string home = "/Users/colly";
	CHECK( reports::Scrub( "open /Users/colly/Documents/show.avc failed", home, "colly" ) ==
	       "open ~/Documents/show.avc failed" );
	// Someone else's home folder, and Windows, and Linux.
	CHECK( reports::Scrub( "/Users/guest/Library/x", home, "colly" ) == "/Users/<user>/Library/x" );
	CHECK( reports::Scrub( "C:\\Users\\Colm Hewson\\AppData", "C:\\Users\\Other", "other" ) ==
	       "C:\\Users\\<user>\\AppData" );
	CHECK( reports::Scrub( "c:/users/sam/file", "", "" ) == "c:/users/<user>/file" );
	CHECK( reports::Scrub( "/home/sam/.config", "", "" ) == "/home/<user>/.config" );
	// The name alone, as a whole word only.
	CHECK( reports::Scrub( "owner colly, not collywobbles", "", "colly" ) == "owner <user>, not collywobbles" );
	// A one-letter name would eat every word it touched; it is left alone.
	CHECK( reports::Scrub( "a b c", "", "a" ) == "a b c" );
	// Anything after ? in a URL.
	CHECK( reports::Scrub( "GET https://example.com/a?key=LT-DATA-1234&email=x@y.z done", "", "" ) ==
	       "GET https://example.com/a done" );
	CHECK( reports::Scrub( "nothing to remove", home, "colly" ) == "nothing to remove" );
}

TEST( TruncationNeverSplitsAUtf8Character )
{
	const std::string text = "ab\xC3\xA9\xE2\x80\x94z";  // a b é — z
	CHECK( reports::Truncate( text, 3 ) == "ab" );
	CHECK( reports::Truncate( text, 4 ) == "ab\xC3\xA9" );
	CHECK( reports::Truncate( text, 6 ) == "ab\xC3\xA9" );
	CHECK( reports::Truncate( text, 100 ) == text );
}

TEST( TheCrashPayloadCarriesTheContractsFieldsWithinItsLimits )
{
	reports::CrashReport report;
	report.version    = "1.0.0";
	report.os         = "macos";
	report.osVersion  = std::string( 100, '9' );
	report.arch       = "arm64";
	report.install    = "3f0c9a52-5d1e-4c1b-9a3e-2b7f0d8c1e44";
	report.kind       = "unclean-exit";
	report.summary    = std::string( 1000, 's' );
	report.detail     = std::string( 100000, 'd' );
	report.signature  = std::string( 500, 'g' );
	report.occurredAt = "2026-09-24T02:10:00Z";

	const std::string body   = reports::CrashPayload( report );
	const auto        fields = licence::json::ParseObject( body );
	CHECK( !fields.empty() );
	CHECK( body.size() <= reports::MAX_BODY );
	CHECK( OnlyContractFields( fields ) );
	CHECK( fields.at( "product" ).text == "datamosh" );
	CHECK( fields.at( "version" ).text == "1.0.0" );
	CHECK( fields.at( "kind" ).text == "unclean-exit" );
	CHECK( fields.at( "install" ).text == report.install );
	CHECK( fields.at( "summary" ).text.size() == reports::MAX_SUMMARY );
	CHECK( fields.at( "osVersion" ).text.size() == reports::MAX_OS_VERSION );
	CHECK( fields.at( "signature" ).text.size() == reports::MAX_SIGNATURE );
	CHECK( fields.at( "detail" ).text.size() <= reports::MAX_DETAIL );
	CHECK( fields.count( "note" ) == 0 );

	// An install id that is not one is left out, not sent mangled.
	report.install = "not/an id";
	CHECK( licence::json::ParseObject( reports::CrashPayload( report ) ).count( "install" ) == 0 );
	// And an empty optional field is absent rather than "".
	report.osVersion.clear();
	CHECK( licence::json::ParseObject( reports::CrashPayload( report ) ).count( "osVersion" ) == 0 );
}

TEST( ResponsesAreSortedIntoSentDroppedAndKept )
{
	CHECK( reports::Classify( Respond( 202, "{\"ok\":true,\"id\":\"x\"}" ) ) == reports::Outcome::Sent );
	CHECK( reports::Classify( Respond( 400, "{\"ok\":false}" ) ) == reports::Outcome::Drop );
	CHECK( reports::Classify( Respond( 413, "{\"ok\":false}" ) ) == reports::Outcome::Drop );
	CHECK( reports::Classify( Respond( 429, "{\"ok\":false}" ) ) == reports::Outcome::Keep );
	CHECK( reports::Classify( Respond( 503, "" ) ) == reports::Outcome::Keep );
	CHECK( reports::Classify( Unreachable() ) == reports::Outcome::Keep );
}

TEST( TheQueueKeepsTheNewestTwenty )
{
	TempFolder     folder;
	reports::Queue queue( folder.Path() / "queue" );
	for( int i = 0; i < 25; ++i )
	{
		char stem[ 16 ];
		std::snprintf( stem, sizeof( stem ), "r%02d", i );
		CHECK( queue.Add( "{\"n\":" + std::to_string( i ) + "}", stem ) );
	}
	const auto files = queue.Files();
	CHECK( files.size() == reports::QUEUE_LIMIT );
	CHECK( !files.empty() && files.front().filename() == "r05.json" );
	CHECK( !files.empty() && files.back().filename() == "r24.json" );
}

TEST( InstallIdsAreRandomAndWellFormed )
{
	const std::string a = reports::NewInstallId();
	const std::string b = reports::NewInstallId();
	CHECK( a != b );
	CHECK( a.size() == 36 && a[ 14 ] == '4' );
	CHECK( reports::ValidInstallId( a ) );
	CHECK( !reports::ValidInstallId( "" ) );
	CHECK( !reports::ValidInstallId( "has space" ) );
	CHECK( !reports::ValidInstallId( std::string( 65, 'a' ) ) );
}

TEST( TheServiceAddressCanBeOverriddenForTests )
{
	const auto live = licence::ParseServiceUrl( "https://letissier.ie" );
	CHECK( live.valid && live.secure && live.host == "letissier.ie" && live.port == 443 && live.basePath.empty() );
	const auto local = licence::ParseServiceUrl( "http://localhost:3000/base/" );
	CHECK( local.valid && !local.secure && local.host == "localhost" && local.port == 3000 &&
	       local.basePath == "/base" );
	CHECK( !licence::ParseServiceUrl( "ftp://x" ).valid );
	CHECK( !licence::ParseServiceUrl( "https://user@host" ).valid );
	CHECK( !licence::ParseServiceUrl( "https://host:99999" ).valid );

	CHECK( licence::ServiceUrl() == "https://letissier.ie" );
	setenv( "LETISSIER_API", "http://localhost:3000/", 1 );
	CHECK( licence::ServiceUrl() == "http://localhost:3000" );
	setenv( "LETISSIER_API", "not a url", 1 );
	CHECK( licence::ServiceUrl() == "https://letissier.ie" );
	unsetenv( "LETISSIER_API" );

	CHECK( reports::FeedbackUrl( "https://letissier.ie", "1.0.0" ) ==
	       "https://letissier.ie/feedback?product=datamosh&version=1.0.0" );
	CHECK( reports::FeedbackUrl( "https://letissier.ie", "1.0.0+a b" ) ==
	       "https://letissier.ie/feedback?product=datamosh&version=1.0.0%2Ba%20b" );
	CHECK( reports::UserAgent( "1.0.0" ) == std::string( "Datamosh/1.0.0 (" ) + reports::OsName() + ")" );
}

// ---------------------------------------------------------------------------
// Consent
// ---------------------------------------------------------------------------

TEST( WithTheSettingOffAndNoAnswerNothingIsEverSent )
{
	// The rule the whole feature stands on. A crash from last time, a caught
	// exception this time, the worker running for two simulated days with the
	// network up — and not one request to the crash endpoint.
	TempFolder folder;
	Clock      clock;
	PlantDeadMarker( folder.Path(), DEAD_PID, "motion-search" );
	TestLicence rig( folder.Path(), licence::Policy::Open, clock,
	                 []( const FakeServer::Request& ) { return Respond( 202, "{\"ok\":true,\"id\":\"r1\"}" ); } );

	rig.service->Pump();
	rig.service->ReportCaught( "a frame", "std::bad_alloc" );
	for( int hour = 0; hour < 48; ++hour )
	{
		clock.now += 3600;
		rig.service->Pump();
	}

	for( const auto& request : rig.server->requests )
		CHECK( request.path != reports::CRASH_PATH );
	CHECK( rig.service->GetReporter()->Asking() );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 0 );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "pending" ) == 2 );
	// The marker it learned that from is gone, so it is not reported twice.
	CHECK( JsonFilesIn( crash::RunningFolder( folder.Path() / "reports" ) ) == 0 );
}

TEST( AfterACrashTheLicenceFieldAsksOnceAndSendSendsIt )
{
	TempFolder folder;
	Clock      clock;
	PlantDeadMarker( folder.Path(), DEAD_PID, "motion-search" );
	TestLicence rig( folder.Path(), licence::Policy::Open, clock,
	                 []( const FakeServer::Request& ) { return Respond( 202, "{\"ok\":true,\"id\":\"r1\"}" ); } );

	rig.service->Pump();
	CHECK( rig.service->Label() == "Licence: unlicensed | closed unexpectedly last time - type send or discard" );
	const std::string readme = Slurp( folder.Path() / "README.txt" );
	CHECK( readme.find( "Datamosh closed unexpectedly last time. Send a crash report to LeTissier" ) !=
	       std::string::npos );
	CHECK( readme.find( "Send crash reports automatically: off" ) != std::string::npos );

	rig.service->Submit( "send" );
	rig.service->Pump();

	std::vector< FakeServer::Request > sent;
	for( const auto& request : rig.server->requests )
		if( request.path == reports::CRASH_PATH )
			sent.push_back( request );
	CHECK( sent.size() == 1 );
	if( sent.size() == 1 )
	{
		const auto& fields = sent[ 0 ].fields;
		CHECK( OnlyContractFields( fields ) );
		CHECK( fields.at( "product" ).text == "datamosh" );
		CHECK( fields.at( "kind" ).text == "unclean-exit" );
		CHECK( fields.at( "os" ).text == reports::OsName() );
		CHECK( fields.at( "osVersion" ).text == "14.6" );
		CHECK( fields.at( "summary" ).text.find( "motion-search" ) != std::string::npos );
		CHECK( fields.at( "detail" ).text.find( "Resolume Arena 7.22.1" ) != std::string::npos );
		CHECK( fields.at( "detail" ).text.find( "1920x1080" ) != std::string::npos );
		CHECK( reports::ValidInstallId( fields.at( "install" ).text ) );
		CHECK( fields.at( "signature" ).text == reports::Signature( "unclean-exit", "Datamosh", "motion-search" ) );
		// Nothing that identifies the person or the machine.
		CHECK( sent[ 0 ].body.find( rig.fingerprint ) == std::string::npos );
		CHECK( sent[ 0 ].body.find( rig.machineHash ) == std::string::npos );
		CHECK( sent[ 0 ].options.timeoutSeconds == 8 );
		CHECK( sent[ 0 ].options.userAgent == reports::UserAgent( DATAMOSH_VERSION ) );
	}
	CHECK( !rig.service->GetReporter()->Asking() );
	CHECK( rig.service->Label() == "Licence: report sent, thank you" );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 0 );
}

TEST( AQuestionIsAskedOnceAndDiscardIsNo )
{
	TempFolder folder;
	Clock      clock;
	PlantDeadMarker( folder.Path(), DEAD_PID, "composite" );
	{
		TestLicence rig( folder.Path(), licence::Policy::Open, clock );
		rig.service->Pump();
		CHECK( rig.service->GetReporter()->Asking() );
		rig.service->Submit( "discard" );
		rig.service->Pump();
		CHECK( !rig.service->GetReporter()->Asking() );
		CHECK( JsonFilesIn( folder.Path() / "reports" / "pending" ) == 0 );
	}

	// Unanswered: the next launch — another process — does not ask again.
	PlantDeadMarker( folder.Path(), DEAD_PID + 1, "mosh" );
	{
		TestLicence rig( folder.Path(), licence::Policy::Open, clock );
		rig.service->Pump();
		CHECK( rig.service->GetReporter()->Asking() );
	}
	// Move the question to a process that has ended, as a restart would.
	std::vector< fs::path > asked;
	std::error_code         error;
	for( fs::directory_iterator it( folder.Path() / "reports" / "pending", error ), end; !error && it != end;
	     it.increment( error ) )
		asked.push_back( it->path() );
	CHECK( asked.size() == 1 );
	for( const fs::path& path : asked )
	{
		const std::string name = path.filename().string();
		fs::rename( path, path.parent_path() / ( std::to_string( DEAD_PID ) + name.substr( name.find( '-' ) ) ) );
	}
	{
		TestLicence rig( folder.Path(), licence::Policy::Open, clock,
		                 []( const FakeServer::Request& ) { return Respond( 202, "{\"ok\":true}" ); } );
		rig.service->Pump();
		CHECK( !rig.service->GetReporter()->Asking() );
		CHECK( JsonFilesIn( folder.Path() / "reports" / "pending" ) == 0 );
		clock.now += 3600;
		rig.service->Pump();
		CHECK( rig.server->requests.empty() );
	}
}

TEST( AlwaysSendQueuesSendsAndKeepsWhatTheSiteCannotTakeYet )
{
	TempFolder folder;
	Clock      clock;
	int        answer = 429;
	TestLicence rig( folder.Path(), licence::Policy::Open, clock, [ &answer ]( const FakeServer::Request& ) {
		return Respond( answer, answer == 202 ? "{\"ok\":true,\"id\":\"r\"}" : "{\"ok\":false,\"error\":\"rate_limited\"}" );
	} );
	rig.service->Pump();
	rig.service->Submit( "always send" );
	rig.service->Pump();
	CHECK( rig.service->GetReporter()->Automatic() );
	CHECK( Slurp( folder.Path() / "reports" / "settings.txt" ).find( "send-automatically=yes" ) != std::string::npos );

	// A caught exception is queued without a question. The person has just
	// typed "always send", so it goes on the next pump; the site is busy, so
	// it stays queued and is not retried until the next launch.
	rig.service->ReportCaught( "a frame", "std::bad_alloc at /Users/colly/x" );
	rig.service->Pump();
	CHECK( !rig.service->GetReporter()->Asking() );
	CHECK( rig.server->requests.size() == 1 );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 1 );
	clock.now += 3600;
	rig.service->Pump();
	CHECK( rig.server->requests.size() == 1 );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 1 );
	if( !rig.server->requests.empty() )
	{
		const auto& fields = rig.server->requests[ 0 ].fields;
		CHECK( fields.at( "kind" ).text == "exception" );
		CHECK( fields.at( "summary" ).text.find( "/Users/<user>/x" ) != std::string::npos );
		CHECK( fields.at( "summary" ).text.find( "colly" ) == std::string::npos );
		CHECK( !fields.at( "occurredAt" ).text.empty() );
	}

	// The same failure again is not a second report.
	rig.service->ReportCaught( "a frame", "std::bad_alloc at /Users/colly/x" );
	rig.service->Pump();
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 1 );

	// Next launch, the site takes it.
	answer = 202;
	TestLicence next( folder.Path(), licence::Policy::Open, clock, [ &answer ]( const FakeServer::Request& ) {
		return Respond( answer, "{\"ok\":true,\"id\":\"r\"}" );
	} );
	next.service->Pump();
	// Never while a composition is loading: nothing until the launch delay.
	clock.now += reports::SEND_DELAY - 1;
	next.service->Pump();
	CHECK( next.server->requests.empty() );
	clock.now += 2;
	next.service->Pump();
	CHECK( next.server->requests.size() == 1 );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 0 );

	// Off is off, including anything still waiting.
	answer = 503;
	next.service->ReportCaught( "SetFloatParameter", "boom" );
	next.service->Pump();
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 1 );
	next.service->Submit( "reports off" );
	next.service->Pump();
	CHECK( !next.service->GetReporter()->Automatic() );
	CHECK( JsonFilesIn( folder.Path() / "reports" / "queue" ) == 0 );
	CHECK( next.service->Label() == "Licence: crash reports off" );
}

TEST( TheInstallIdIsMadeOnceAndKept )
{
	TempFolder folder;
	Clock      clock;
	std::string first;
	{
		TestLicence rig( folder.Path(), licence::Policy::Open, clock );
		rig.service->Pump();
		first = rig.service->GetReporter()->InstallId();
	}
	CHECK( reports::ValidInstallId( first ) );
	TestLicence again( folder.Path(), licence::Policy::Open, clock );
	again.service->Pump();
	CHECK( again.service->GetReporter()->InstallId() == first );
	CHECK( Slurp( folder.Path() / "install-id" ).find( first ) == 0 );
	// Not derived from the machine.
	CHECK( first.find( again.fingerprint ) == std::string::npos );
}

// ---------------------------------------------------------------------------
// The marker itself
// ---------------------------------------------------------------------------

#if !defined( _WIN32 )
TEST( AProcessThatDiesMidFrameLeavesAMarkerTheNextLoadFinds )
{
	// A real death, not a planted file: a child process maps the marker,
	// enters a frame, reaches a pass, and is killed outright — no destructor,
	// no Leave, nothing flushed by hand. What it wrote to the mapping must
	// still be there for the parent to read.
	TempFolder     folder;
	const fs::path reportsFolder = folder.Path() / "reports";

	auto dieIn = [ & ]( bool leaveFirst ) -> pid_t {
		const pid_t child = fork();
		if( child == 0 )
		{
			if( !crash::Open( reportsFolder, "Datamosh", "1.0.0" ) )
				_exit( 3 );
			crash::SetHost( "Resolume Arena", "7.22.1" );
			Breadcrumb breadcrumb;
			breadcrumb.Bind( crash::Claim() );
			breadcrumb.Enter( "frame" );
			breadcrumb.Rendered( 640, 360 );
			breadcrumb.Stage( "motion-search" );
			if( leaveFirst )
				breadcrumb.Leave();
			kill( getpid(), SIGKILL );
			_exit( 4 );
		}
		int status = 0;
		waitpid( child, &status, 0 );
		return child;
	};

	const pid_t died = dieIn( false );
	auto findings    = crash::Harvest( reportsFolder, "Datamosh", crash::ProcessAlive, crash::CurrentPid() );
	CHECK( findings.size() == 1 );
	if( findings.size() == 1 )
	{
		CHECK( findings[ 0 ].stage == "motion-search" );
		CHECK( findings[ 0 ].host == "Resolume Arena 7.22.1" );
		CHECK( findings[ 0 ].version == "1.0.0" );
		CHECK( findings[ 0 ].width == 640 );
		CHECK( findings[ 0 ].frames == 1 );
		CHECK( findings[ 0 ].pid == static_cast< std::uint32_t >( died ) );
	}
	// Harvested markers are removed, so the same death is one report.
	CHECK( crash::Harvest( reportsFolder, "Datamosh", crash::ProcessAlive, crash::CurrentPid() ).empty() );

	// Died outside our call: not ours, no finding — and the marker still goes.
	dieIn( true );
	CHECK( crash::Harvest( reportsFolder, "Datamosh", crash::ProcessAlive, crash::CurrentPid() ).empty() );
	CHECK( fs::is_empty( crash::RunningFolder( reportsFolder ) ) );
}
#endif

TEST( ALiveProcessesMarkerIsNeverReported )
{
	TempFolder folder;
	PlantDeadMarker( folder.Path(), crash::CurrentPid(), "mosh" );
	PlantDeadMarker( folder.Path(), DEAD_PID, "mosh", "DatamoshTransplant" );
	const fs::path reportsFolder = folder.Path() / "reports";
	auto alive = []( std::uint32_t pid ) { return pid == crash::CurrentPid(); };
	// This process's own marker: still running, left alone.
	CHECK( crash::Harvest( reportsFolder, "Datamosh", alive, crash::CurrentPid() ).empty() );
	CHECK( fs::exists( crash::RunningFolder( reportsFolder ) /
	                   ( "Datamosh-" + std::to_string( crash::CurrentPid() ) + ".mark" ) ) );
	// Each binary reads only its own markers.
	CHECK( crash::Harvest( reportsFolder, "DatamoshTransplant", alive, crash::CurrentPid() ).size() == 1 );
}

namespace {

template< typename PluginType >
struct Observed : PluginType
{
	using PluginType::breadcrumb;
	using PluginType::createdAt;
	using PluginType::feedbackParamIndex;
	using PluginType::licenceParamIndex;
};

/// Looks at its own slot from inside a frame, where a death would be ours.
struct Peeking : Observed< DatamoshEffect >
{
	mutable std::uint32_t inFrameDuring = 99;
	mutable std::string   stageDuring;

protected:
	void AdjustParams( MoshParams& ) const override
	{
		if( const BreadcrumbSlot* slot = this->breadcrumb.Bound() )
		{
			inFrameDuring = slot->inFrame;
			stageDuring   = slot->stage;
		}
	}
};

}  // namespace

TEST( ThePluginMarksEachCallAndClearsItOnTheWayOut )
{
	TempFolder folder;
	Clock      clock;
	TestLicence rig( folder.Path(), licence::Policy::Open, clock );
	rig.service->Pump();
	ScopedLicence scoped( *rig.service );
	licence::testing::SetReportsFolder( folder.Path() / "reports" );

	InputTexture  texture;
	RenderTarget  output;
	CHECK( texture.Create( 64, 48 ) );
	CHECK( output.Allocate( 64, 48, GL_RGBA8 ) );
	texture.Upload( MakeShiftedPattern( 64, 48, 0.0f, 0.0f ) );
	FFGLTextureStruct  descriptor{};
	descriptor.Width = descriptor.HardwareWidth = 64;
	descriptor.Height = descriptor.HardwareHeight = 48;
	descriptor.Handle = texture.GetHandle();
	FFGLTextureStruct* inputs[ 1 ] = { &descriptor };

	{
		Peeking            plugin;
		FFGLViewportStruct viewport{ 0, 0, 64, 48 };
		plugin.SetHostInfo( "Resolume Arena", "7.22.1" );
		CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );
		// InitGL runs on the host's render thread: it must not have touched
		// the disk. The marker is the worker's to create.
		CHECK( !fs::exists( crash::RunningFolder( folder.Path() / "reports" ) ) );
		CHECK( plugin.breadcrumb.Bound() == nullptr );

		ProcessOpenGLStruct frame{};
		frame.numInputTextures = 1;
		frame.inputTextures    = inputs;
		frame.HostFBO          = output.GetFBO();

		// A frame before the marker exists renders normally, unrecorded.
		plugin.SetTime( 0.0 );
		CHECK( plugin.ProcessOpenGL( &frame ) == FF_SUCCESS );
		CHECK( plugin.breadcrumb.Bound() == nullptr );

		// What the worker does when it starts; the next frame claims a slot.
		licence::testing::OpenCrashMarker();
		CHECK( crash::IsOpen() );
		for( int i = 1; i <= 3; ++i )
		{
			plugin.SetTime( i / 60.0 );
			CHECK( plugin.ProcessOpenGL( &frame ) == FF_SUCCESS );
		}
		// Inside the call, the marker says so: this is what a death leaves.
		CHECK( plugin.inFrameDuring == 1 );
		BreadcrumbSlot* slot = plugin.breadcrumb.Bound();
		CHECK( slot != nullptr );
		if( slot )
		{
			// Out of the call: not in a frame. The passes wrote their names on
			// the way through, so the last one reached is still there.
			CHECK( slot->inFrame == 0 );
			CHECK( slot->frames == 3 );
			CHECK( slot->width == 64 && slot->height == 48 );
			CHECK( std::string( slot->stage ) == "composite" );
		}

		// Were the host to die now, mid-pass, the file on disk would say so —
		// read back through the file, not through our own pointer.
		plugin.breadcrumb.Enter( "motion-search" );
		auto findings = crash::Harvest( folder.Path() / "reports", "Datamosh",
		                                []( std::uint32_t ) { return false; }, 0 );
		CHECK( findings.size() == 1 );
		if( !findings.empty() )
		{
			CHECK( findings[ 0 ].stage == "motion-search" );
			CHECK( findings[ 0 ].host == "Resolume Arena 7.22.1" );
			CHECK( findings[ 0 ].version == DATAMOSH_VERSION );
		}
		plugin.breadcrumb.Leave();
		CHECK( plugin.DeInitGL() == FF_SUCCESS );
	}

	licence::testing::SetReportsFolder( {} );
	crash::testing::Close();
	texture.Release();
	output.Release();
}

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

TEST( SendFeedbackOpensTheFeedbackPageOncePerPress )
{
	TempFolder folder;
	Clock      clock;
	TestLicence rig( folder.Path(), licence::Policy::Lock, clock );
	rig.service->Pump();
	ScopedLicence scoped( *rig.service );

	auto check = [ & ]( auto& plugin ) {
		const unsigned int index = plugin.feedbackParamIndex;
		CHECK( index != 0xFFFFFFFFu );
		CHECK( plugin.GetParamName( index ) == std::string( "Send Feedback" ) );
		CHECK( plugin.GetParamType( index ) == FF_TYPE_EVENT );
		CHECK( plugin.GetParamGroup( index ) == "Help" );
		// Licence stays last.
		CHECK( plugin.licenceParamIndex == plugin.GetNumParams() - 1 );
		CHECK( index == plugin.licenceParamIndex - 1 );

		rig.opened->clear();
		// A brand-new instance is a composition loading, not a person.
		plugin.SetFloatParameter( index, 1.0f );
		plugin.SetFloatParameter( index, 0.0f );
		rig.service->Pump();
		CHECK( rig.opened->empty() );

		plugin.createdAt -= std::chrono::seconds( 10 );
		plugin.SetFloatParameter( index, 1.0f );
		plugin.SetFloatParameter( index, 1.0f );  // held, not a second press
		plugin.SetFloatParameter( index, 0.0f );  // the release
		rig.service->Pump();
		CHECK( rig.opened->size() == 1 );
		if( !rig.opened->empty() )
			CHECK( rig.opened->front() == "https://letissier.ie/feedback?product=datamosh&version=" DATAMOSH_VERSION );

		// A double click is one page.
		plugin.SetFloatParameter( index, 1.0f );
		plugin.SetFloatParameter( index, 0.0f );
		rig.service->Pump();
		CHECK( rig.opened->size() == 1 );
		clock.now += reports::FEEDBACK_DEBOUNCE + 1;
	};

	Observed< DatamoshEffect > effect;
	check( effect );
	Observed< DatamoshMixer > mixer;
	check( mixer );

	// The word works too, typed where everything else is typed.
	rig.opened->clear();
	rig.service->Submit( "feedback" );
	rig.service->Pump();
	CHECK( rig.opened->size() == 1 );
	// And none of it touched the licence.
	CHECK( rig.service->Label() == "Licence: locked, unlicensed" );
}

}  // namespace datamosh::test
