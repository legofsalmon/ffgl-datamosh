// Tests for licensing — the decision, the wire, the folder, the worker.
//
// The rule from CLAUDE.md applies with extra force here: a licence check that
// has quietly stopped checking looks exactly like one that passes. So these
// drive the service's own signed vectors, the real request bodies and the real
// files, and the policy table covers every status under every policy.

#include "harness/Licence.h"
#include "harness/TestRunner.h"

#include <Json.h>
#include <Licence.h>
#include <Service.h>
#include <Store.h>
#include <Wire.h>
#include <sdk/licence.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace datamosh::test {

using licence::Gate;
using licence::InputKind;
using licence::Mark;
using licence::Policy;
using licence::Restriction;
using licence::Status;

namespace {

std::string ReadFile( const std::string& path )
{
	std::ifstream      file( path, std::ios::binary );
	std::ostringstream text;
	text << file.rdbuf();
	return text.str();
}

std::string StringField( const std::map< std::string, licence::json::Value >& object, const char* name )
{
	const auto found = object.find( name );
	return found == object.end() ? std::string() : found->second.text;
}

std::int64_t NumberField( const std::map< std::string, licence::json::Value >& object, const char* name )
{
	return std::strtoll( StringField( object, name ).c_str(), nullptr, 10 );
}

Status StatusNamed( const std::string& name )
{
	for( Status status : { Status::Active, Status::UpdateRequired, Status::CheckInRequired, Status::Expired,
	                       Status::WrongMachine, Status::Invalid } )
	{
		if( name == letissier::to_string( status ) )
			return status;
	}
	return Status::Invalid;
}

const Status ALL_STATUSES[] = { Status::Active,  Status::UpdateRequired, Status::CheckInRequired,
                                Status::Expired, Status::WrongMachine,   Status::Invalid };

/// A server that behaves like the real one: it hashes the machine it is sent,
/// echoes that hash, and mints a token bound to it.
FakeServer::Handler HonestServer( Clock& clock, const std::string& edition = "standard",
                                  std::int64_t lease = 30 * 86400 )
{
	return [ &clock, edition, lease ]( const FakeServer::Request& request ) {
		const std::string sent = StringField( request.fields, "machine" );
		const std::string hash = letissier::machine_hash( sent );

		TokenSpec spec;
		spec.machine    = hash;
		spec.edition    = edition;
		spec.exp        = clock.now + lease;
		spec.maintUntil = clock.now + 365 * 86400;
		spec.iat        = clock.now;
		const std::string key = request.path == licence::PATH_TRIAL ? std::string( "LT-DATA-TR1A-L000-0000" )
		                                                            : StringField( request.fields, "key" );
		spec.key = key;

		return Respond( 200, "{\"ok\":true,\"token\":" + licence::json::Quote( MintToken( spec ) ) +
		                         ",\"machine\":" + licence::json::Quote( hash ) + ",\"key\":" +
		                         licence::json::Quote( key ) +
		                         ",\"product\":\"datamosh\",\"edition\":\"standard\",\"seats\":2,\"seatsUsed\":1,"
		                         "\"checkInBy\":\"2026-10-24T00:00:00.000Z\","
		                         "\"maintenanceUntil\":\"2027-09-24T00:00:00.000Z\"}" );
	};
}

}  // namespace

// ---------------------------------------------------------------------------
// The SDK against the service's own vectors
// ---------------------------------------------------------------------------

TEST( LicenceSdkAgreesWithTheServicesVectors )
{
	// tests/data/licence-vectors.json is letissier.ie/integrate/vectors.json,
	// verbatim. "Every SDK must agree with these." Read from the file rather
	// than retyped, so a refreshed copy is a refreshed test.
	const std::string text = ReadFile( DATAMOSH_TEST_DATA_DIR "/licence-vectors.json" );
	const auto        root = licence::json::ParseObject( text );
	CHECK( !root.empty() );

	const std::string  key         = StringField( root, "publicKeyHex" );
	const std::string  fingerprint = StringField( root, "fingerprint" );
	const std::int64_t now         = NumberField( root, "now" );
	const auto         tokens      = licence::json::ParseObject( StringField( root, "tokens" ) );
	const auto         claims      = licence::json::ParseObject( StringField( root, "claims" ) );
	const std::string  product     = StringField( claims, "product" );
	const std::int64_t iat         = NumberField( claims, "iat" );

	CHECK( letissier::machine_hash( fingerprint ) == StringField( root, "machineHash" ) );
	CHECK( product == "vizz" );

	letissier::Claims out;
	CHECK( letissier::verify( StringField( tokens, "valid" ), key, out ) );
	CHECK( out.seats == 2 );
	CHECK( out.product == "vizz" );
	CHECK( letissier::verify( StringField( tokens, "validTrial" ), key, out ) );
	CHECK( !letissier::verify( StringField( tokens, "tampered" ), key, out ) );
	CHECK( !letissier::verify( StringField( tokens, "wrongKey" ), key, out ) );
	CHECK( !letissier::verify( StringField( tokens, "malformed" ), key, out ) );

	auto statusOf = [ & ]( const char* token, std::int64_t build, std::int64_t at ) {
		// Through Decide, the product check included, with the vectors' own
		// product — so this is the path the plugin takes.
		return licence::Decide( StringField( tokens, token ), fingerprint, build, at, key, product ).status;
	};

	int cases = 0;
	for( const std::string& item : licence::json::ParseArray( StringField( root, "entitlement" ) ) )
	{
		const auto entry = licence::json::ParseObject( item );
		CHECK( statusOf( "valid", NumberField( entry, "buildDate" ), now ) == StatusNamed( StringField( entry, "expect" ) ) );
		++cases;
	}
	for( const std::string& item : licence::json::ParseArray( StringField( root, "lease" ) ) )
	{
		const auto entry = licence::json::ParseObject( item );
		CHECK( statusOf( "valid", iat, NumberField( entry, "at" ) ) == StatusNamed( StringField( entry, "expect" ) ) );
		++cases;
	}
	for( const std::string& item : licence::json::ParseArray( StringField( root, "trialLease" ) ) )
	{
		const auto entry = licence::json::ParseObject( item );
		CHECK( statusOf( "validTrial", iat, NumberField( entry, "at" ) ) == StatusNamed( StringField( entry, "expect" ) ) );
		++cases;
	}
	// Three entitlement, two lease, one trial: a vectors file that parsed to
	// nothing must not pass by asserting nothing.
	CHECK( cases == 6 );

	CHECK( statusOf( "tampered", iat, now ) == Status::Invalid );
	CHECK( statusOf( "wrongKey", iat, now ) == Status::Invalid );
	CHECK( statusOf( "malformed", iat, now ) == Status::Invalid );
	CHECK( licence::Decide( StringField( tokens, "valid" ), "SOME-OTHER-MACHINE", iat, now, key, product ).status ==
	       Status::WrongMachine );
}

TEST( LicenceSha256MatchesTheStandard )
{
	// The Monocypher backend brings its own SHA-256, so it gets the FIPS 180-4
	// examples: empty, one block, two blocks, and a million bytes.
	CHECK( letissier::machine_hash( "abc" ) == "ba7816bf8f01cfea414140de5dae2223" );
	CHECK( letissier::machine_hash( "" ) == "e3b0c44298fc1c149afbf4c8996fb924" );
	CHECK( letissier::machine_hash( "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" ) ==
	       "248d6a61d20638b8e5c026930c3e6039" );
	CHECK( letissier::machine_hash( std::string( 1000000, 'a' ) ) == "cdc76e5c9914fb9281a1c7e284d73e67" );
	// Trimmed before hashing, as the service does.
	CHECK( letissier::machine_hash( "  abc\n" ) == letissier::machine_hash( "abc" ) );
}

TEST( TheLiveKeyIsCompiledIn )
{
	// A build with no key restricts nothing — by design — which also means a
	// build that lost its key would look fine and check nothing.
	CHECK( licence::PublicKeyConfigured( licence::PublicKey() ) );
	CHECK( licence::PublicKey() == "1fca6c21f2eb7963fd646272a731a41a191d3a4cda839e295c5cda67978fcc85" );
	CHECK( licence::BuildDate() > 1'700'000'000 );
	CHECK( licence::BuildDate() < 4'000'000'000 );
	CHECK( std::string( licence::PRODUCT ) == "datamosh" );
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

TEST( ATokenForAnotherProductIsNoLicenceHere )
{
	// The SDK checks signature, machine and dates, but not product. A Vizz
	// licence signed by the same studio key must not license Datamosh.
	const std::string text   = ReadFile( DATAMOSH_TEST_DATA_DIR "/licence-vectors.json" );
	const auto        root   = licence::json::ParseObject( text );
	const auto        tokens = licence::json::ParseObject( StringField( root, "tokens" ) );
	const auto        verdict =
		licence::Decide( StringField( tokens, "valid" ), StringField( root, "fingerprint" ), 1'760'000'000,
	                     NumberField( root, "now" ), StringField( root, "publicKeyHex" ), "datamosh" );
	CHECK( verdict.status == Status::Invalid );
	CHECK( !verdict.hasClaims );

	// And with a token the tests mint: same machine, same key, other product.
	TokenSpec spec;
	spec.product    = "vizz";
	spec.machine    = letissier::machine_hash( "RIG" );
	spec.exp        = 2'000'000'000;
	spec.maintUntil = 2'000'000'000;
	const auto minted =
		licence::Decide( MintToken( spec ), "RIG", 1'760'000'000, 1'760'086'400, TestPublicKeyHex(), "datamosh" );
	CHECK( minted.status == Status::Invalid );
	// Invalid, not WrongMachine: nobody should be sent to re-activate a key
	// that can never work here.
	spec.machine = letissier::machine_hash( "SOMEWHERE-ELSE" );
	CHECK( licence::Decide( MintToken( spec ), "RIG", 1'760'000'000, 1'760'086'400, TestPublicKeyHex(), "datamosh" )
	           .status == Status::Invalid );

	// Unusable under every restricting policy.
	CHECK( licence::RestrictionFor( minted.status, Policy::Watermark, true ) == Restriction::Watermark );
	CHECK( licence::RestrictionFor( minted.status, Policy::Lock, true ) == Restriction::Lock );

	// The matching product does verify — so the refusals above are the
	// product check and not a broken minting helper.
	spec.product = "datamosh";
	spec.machine = letissier::machine_hash( "RIG" );
	CHECK( licence::Decide( MintToken( spec ), "RIG", 1'760'000'000, 1'760'086'400, TestPublicKeyHex(), "datamosh" )
	           .status == Status::Active );
}

TEST( ALapsedTrialIsRestrictedOnlyWhenThePolicySaysSo )
{
	TokenSpec trial;
	trial.edition    = "trial";
	trial.machine    = letissier::machine_hash( "RIG" );
	trial.exp        = 1'761'000'000;
	trial.maintUntil = trial.exp;
	const std::string token = MintToken( trial );

	const auto during = licence::Decide( token, "RIG", 1'760'000'000, trial.exp - 1, TestPublicKeyHex(), "datamosh" );
	CHECK( during.status == Status::Active );
	const auto after = licence::Decide( token, "RIG", 1'760'000'000, trial.exp + 1, TestPublicKeyHex(), "datamosh" );
	// Expired, not CheckInRequired: a trial's lease is its whole life.
	CHECK( after.status == Status::Expired );

	CHECK( licence::RestrictionFor( after.status, Policy::Open, true ) == Restriction::None );
	CHECK( licence::RestrictionFor( after.status, Policy::Watermark, true ) == Restriction::Watermark );
	CHECK( licence::RestrictionFor( after.status, Policy::Lock, true ) == Restriction::Lock );
	CHECK( licence::MarkFor( after.status ) == Mark::TrialEnded );

	// The same lapse on a bought licence only asks for a check-in.
	TokenSpec bought = trial;
	bought.edition   = "standard";
	const auto lapsed =
		licence::Decide( MintToken( bought ), "RIG", 1'760'000'000, trial.exp + 1, TestPublicKeyHex(), "datamosh" );
	CHECK( lapsed.status == Status::CheckInRequired );
	CHECK( licence::RestrictionFor( lapsed.status, Policy::Lock, true ) == Restriction::None );
}

TEST( ThePolicyTableCoversEveryStatus )
{
	// The whole table, so flipping POLICY is a one-line change into behaviour
	// that has already been exercised.
	for( Status status : ALL_STATUSES )
	{
		const bool unusable =
			status == Status::Invalid || status == Status::Expired || status == Status::WrongMachine;

		CHECK( licence::RestrictionFor( status, Policy::Open, true ) == Restriction::None );
		CHECK( licence::RestrictionFor( status, Policy::Watermark, true ) ==
		       ( unusable ? Restriction::Watermark : Restriction::None ) );
		CHECK( licence::RestrictionFor( status, Policy::Lock, true ) ==
		       ( unusable ? Restriction::Lock : Restriction::None ) );

		// A build that cannot verify restricts nothing, under any policy.
		for( Policy policy : { Policy::Open, Policy::Watermark, Policy::Lock } )
			CHECK( licence::RestrictionFor( status, policy, false ) == Restriction::None );

		CHECK( ( licence::MarkFor( status ) != Mark::None ) == unusable );
	}

	// Only 64 hex characters count as a key.
	CHECK( !licence::PublicKeyConfigured( "" ) );
	CHECK( !licence::PublicKeyConfigured( "REPLACE_WITH_YOUR_PUBLIC_KEY_HEX" ) );
	CHECK( !licence::PublicKeyConfigured( std::string( 63, 'a' ) ) );
	CHECK( !licence::PublicKeyConfigured( std::string( 64, 'g' ) ) );
	CHECK( licence::PublicKeyConfigured( std::string( 64, 'a' ) ) );
}

TEST( ARunningInstancesGateOnlyEverLoosens )
{
	const Gate undecided{};
	const Gate open{ Restriction::None, Mark::None, true };
	const Gate marked{ Restriction::Watermark, Mark::Unlicensed, true };
	const Gate ended{ Restriction::Watermark, Mark::TrialEnded, true };
	const Gate locked{ Restriction::Lock, Mark::Unlicensed, true };

	// Undecided renders unrestricted, then the first decision is adopted.
	licence::GateLatch fresh;
	CHECK( fresh.Update( undecided ) == Gate{} );
	CHECK( fresh.Update( marked ) == marked );

	// Licensing mid-session takes the mark off at once...
	CHECK( fresh.Update( open ) == open );
	// ...and a licence lapsing mid-session never puts it back.
	CHECK( fresh.Update( marked ) == open );
	CHECK( fresh.Update( locked ) == open );

	// A locked instance unlocks to the mark, then to nothing — never back.
	licence::GateLatch lockedFirst;
	CHECK( lockedFirst.Update( locked ) == locked );
	CHECK( lockedFirst.Update( marked ) == marked );
	CHECK( lockedFirst.Update( locked ) == marked );
	// Nor does the wording change under an operator's feet.
	CHECK( lockedFirst.Update( ended ) == marked );
	// A worker losing its decision (it never does, but) changes nothing.
	CHECK( lockedFirst.Update( undecided ) == marked );
}

TEST( TypedInputIsReadTheWayTheServiceReadsKeys )
{
	using licence::Classify;

	// The service's own folding: upper case, missing or 1T- prefix, I/L→1,
	// O→0, U→V in the body only.
	CHECK( licence::NormaliseKey( "lt-data-k7m2-9pqr-4xtc" ) == "LT-DATA-K7M2-9PQR-4XTC" );
	CHECK( licence::NormaliseKey( "DATA-K7M2-9PQR-4XTC" ) == "LT-DATA-K7M2-9PQR-4XTC" );
	CHECK( licence::NormaliseKey( "1T-DATA-K7M2-9PQR-4XTC" ) == "LT-DATA-K7M2-9PQR-4XTC" );
	CHECK( licence::NormaliseKey( " LT-DATA-KIMO-9PQR-4XTU " ) == "LT-DATA-K1M0-9PQR-4XTV" );

	CHECK( Classify( "lt-data-k7m2-9pqr-4xtc" ).kind == InputKind::Key );
	CHECK( Classify( "lt-data-k7m2-9pqr-4xtc" ).value == "LT-DATA-K7M2-9PQR-4XTC" );
	// Studio-issued keys for this product have the same shape and tag.
	CHECK( Classify( "LT-DATA-TR1A-L000-0000" ).kind == InputKind::Key );
	// A tag nobody knows yet is the service's to judge, not ours.
	CHECK( Classify( "LT-ZZZZ-K7M2-9PQR-4XTC" ).kind == InputKind::Key );

	// The friendly hint: another product's key, named.
	const auto vizz = Classify( "LT-VIZZ-K7M2-9PQR-4XTC" );
	CHECK( vizz.kind == InputKind::ForeignKey );
	CHECK( vizz.product == "Vizz" );
	CHECK( Classify( "LT-CREW-K7M2-9PQR-4XTC" ).product == "Crewbox" );
	CHECK( Classify( "LT-LIGH-K7M2-9PQR-4XTC" ).product == "Light" );
	CHECK( Classify( "LT-YEWE-K7M2-9PQR-4XTC" ).product == "Yewee" );

	CHECK( Classify( "vj@example.com" ).kind == InputKind::Email );
	CHECK( Classify( "  Folder " ).kind == InputKind::Folder );
	CHECK( Classify( "deactivate" ).kind == InputKind::Deactivate );
	CHECK( Classify( "check" ).kind == InputKind::CheckIn );
	CHECK( Classify( "" ).kind == InputKind::Empty );
	CHECK( Classify( "   " ).kind == InputKind::Empty );
	CHECK( Classify( "hello" ).kind == InputKind::Unrecognised );
	CHECK( Classify( "LT-DATA-K7M2" ).kind == InputKind::Unrecognised );

	// A token, including one that arrived wrapped across lines.
	TokenSpec spec;
	spec.machine          = "x";
	const std::string tok = MintToken( spec );
	CHECK( Classify( tok ).kind == InputKind::Token );
	const std::string wrapped = tok.substr( 0, 40 ) + "\r\n  " + tok.substr( 40 );
	CHECK( Classify( wrapped ).kind == InputKind::Token );
	CHECK( Classify( wrapped ).value == tok );
}

TEST( TheServicesErrorShapeIsReadInFull )
{
	// The message is the one thing the operator is meant to read, and it is
	// prose: quotes, escapes, non-ASCII punctuation.
	const auto reply = licence::ParseReply( Respond(
		409, "{\"ok\":false,\"reason\":\"no_seats\",\"message\":\"All 2 seats are in use \\u2014 release one "
		     "at \\\"letissier.ie/account\\\".\"}" ) );
	CHECK( reply.reached );
	CHECK( !reply.ok );
	CHECK( reply.reason == "no_seats" );
	CHECK( reply.message == "All 2 seats are in use \xE2\x80\x94 release one at \"letissier.ie/account\"." );

	// ok:true with an error status is not success.
	CHECK( !licence::ParseReply( Respond( 500, "{\"ok\":true}" ) ).ok );
	// Something that is not the service still produces a sentence.
	const auto proxy = licence::ParseReply( Respond( 502, "<html>Bad gateway</html>" ) );
	CHECK( !proxy.ok );
	CHECK( !proxy.message.empty() );
	// And no reply at all is not a reply.
	CHECK( !licence::ParseReply( Unreachable() ).reached );
}

// ---------------------------------------------------------------------------
// The worker, against a stand-in service and a real folder
// ---------------------------------------------------------------------------

TEST( ActivationSendsTheRawFingerprintAndKeepsTheToken )
{
	TempFolder  folder;
	Clock       clock;
	TestLicence rig( folder.Path(), Policy::Watermark, clock, HonestServer( clock ) );
	licence::Service& service = *rig.service;

	service.Pump();
	CHECK( service.CurrentGate().decided );
	CHECK( service.CurrentGate().restriction == Restriction::Watermark );
	CHECK( service.CurrentGate().mark == Mark::Unlicensed );
	CHECK( service.Label() == "Licence: unlicensed" );

	service.Submit( "lt-data-k7m2-9pqr-4xtc" );
	service.Pump();

	CHECK( rig.server->requests.size() == 1 );
	if( rig.server->requests.size() == 1 )
	{
		const auto& sent = rig.server->requests[ 0 ];
		CHECK( sent.path == licence::PATH_ACTIVATE );
		// RAW. The service hashes what it receives; sending the hash would bind
		// the token to sha256(sha256(id)).
		CHECK( StringField( sent.fields, "machine" ) == rig.fingerprint );
		CHECK( StringField( sent.fields, "machine" ) != rig.machineHash );
		CHECK( StringField( sent.fields, "product" ) == "datamosh" );
		CHECK( StringField( sent.fields, "key" ) == "LT-DATA-K7M2-9PQR-4XTC" );
		CHECK( StringField( sent.fields, "label" ) == "test rig" );
	}

	CHECK( service.CurrentStatus() == Status::Active );
	CHECK( service.CurrentGate().restriction == Restriction::None );
	CHECK( service.Label() == "Licence: active" );

	licence::Store store( folder.Path() );
	CHECK( store.ReadToken().has_value() );
	CHECK( store.ReadKey() == std::optional< std::string >( "LT-DATA-K7M2-9PQR-4XTC" ) );
	// The README carries the request code for offline activation.
	const std::string readme = ReadFile( ( folder.Path() / "README.txt" ).string() );
	CHECK( readme.find( rig.fingerprint ) != std::string::npos );
	CHECK( readme.find( "active" ) != std::string::npos );
}

TEST( ATokenBoundToAnotherMachineIsNeverStored )
{
	// The service as it would behave if the client had pre-hashed: it hashes
	// what it got, so the machine it echoes is sha256 of our hash. The echo is
	// the one moment this is visible, so it must be refused there.
	TempFolder folder;
	Clock      clock;
	auto       doubleHashing = [ &clock ]( const FakeServer::Request& request ) {
        const std::string wrong = letissier::machine_hash( letissier::machine_hash( StringField( request.fields, "machine" ) ) );
        TokenSpec         spec;
        spec.machine    = wrong;
        spec.exp        = clock.now + 86400;
        spec.maintUntil = clock.now + 86400;
        return Respond( 200, "{\"ok\":true,\"token\":" + licence::json::Quote( MintToken( spec ) ) +
                                 ",\"machine\":" + licence::json::Quote( wrong ) + "}" );
	};
	TestLicence rig( folder.Path(), Policy::Watermark, clock, doubleHashing );
	rig.service->Pump();
	rig.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	rig.service->Pump();

	licence::Store store( folder.Path() );
	CHECK( !store.ReadToken().has_value() );
	CHECK( rig.service->CurrentGate().restriction == Restriction::Watermark );
	CHECK( rig.service->Label().find( "nothing was stored" ) != std::string::npos );

	// And an echo that matches while the TOKEN names another machine is
	// refused by the token check behind it.
	TempFolder second;
	auto       lyingToken = [ &clock ]( const FakeServer::Request& request ) {
        const std::string right = letissier::machine_hash( StringField( request.fields, "machine" ) );
        TokenSpec         spec;
        spec.machine    = letissier::machine_hash( "ANOTHER" );
        spec.exp        = clock.now + 86400;
        spec.maintUntil = clock.now + 86400;
        return Respond( 200, "{\"ok\":true,\"token\":" + licence::json::Quote( MintToken( spec ) ) +
                                 ",\"machine\":" + licence::json::Quote( right ) + "}" );
	};
	TestLicence other( second.Path(), Policy::Watermark, clock, lyingToken );
	other.service->Pump();
	other.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	other.service->Pump();
	CHECK( !licence::Store( second.Path() ).ReadToken().has_value() );
}

TEST( TheServicesRefusalIsShownInItsOwnWords )
{
	TempFolder  folder;
	Clock       clock;
	TestLicence rig( folder.Path(), Policy::Watermark, clock, []( const FakeServer::Request& ) {
		return Respond( 409, "{\"ok\":false,\"reason\":\"no_seats\",\"message\":\"All 2 seats are in use.\"}" );
	} );
	rig.service->Pump();
	rig.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	rig.service->Pump();

	CHECK( rig.service->Label() == "Licence: All 2 seats are in use." );
	CHECK( !licence::Store( folder.Path() ).ReadToken().has_value() );
	CHECK( rig.service->CurrentGate().restriction == Restriction::Watermark );
	const std::string readme = ReadFile( ( folder.Path() / "README.txt" ).string() );
	CHECK( readme.find( "All 2 seats are in use." ) != std::string::npos );
}

TEST( ALockedLicenceSaysLockedEvenWhileAnsweringInput )
{
	// Under Lock the name is the only thing that tells a locked instance from
	// a dead one, and an answer to something typed stays up until the next
	// input. So it must not displace the word.
	TempFolder  folder;
	Clock       clock;
	TestLicence rig( folder.Path(), Policy::Lock, clock, []( const FakeServer::Request& ) {
		return Respond( 409, "{\"ok\":false,\"reason\":\"no_seats\",\"message\":\"All 2 seats are in use.\"}" );
	} );
	rig.service->Pump();
	CHECK( rig.service->Label() == "Licence: locked, unlicensed" );

	rig.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	rig.service->Pump();
	CHECK( rig.service->CurrentGate().restriction == Restriction::Lock );
	CHECK( rig.service->Label() == "Licence: locked, All 2 seats are in use." );

	rig.service->Submit( "not a key" );
	rig.service->Pump();
	CHECK( rig.service->Label().rfind( "Licence: locked, type a key", 0 ) == 0 );
}

TEST( AnotherProductsKeyNeverReachesTheService )
{
	// Today the service ignores `product` and would take a Vizz seat on this
	// machine for a token this plugin then refuses.
	TempFolder  folder;
	Clock       clock;
	TestLicence rig( folder.Path(), Policy::Watermark, clock, HonestServer( clock ) );
	rig.service->Pump();
	rig.service->Submit( "LT-VIZZ-K7M2-9PQR-4XTC" );
	rig.service->Pump();
	CHECK( rig.server->requests.empty() );
	CHECK( rig.service->Label() == "Licence: That key is for Vizz, not Datamosh." );
}

TEST( CheckingInPersistsTheNewTokenAndOfflineChangesNothing )
{
	TempFolder folder;
	Clock      clock;
	bool       online = true;
	auto       honest = HonestServer( clock );
	TestLicence rig( folder.Path(), Policy::Watermark, clock, [ & ]( const FakeServer::Request& request ) {
		return online ? honest( request ) : Unreachable();
	} );
	licence::Service& service = *rig.service;
	licence::Store    store( folder.Path() );

	service.Pump();
	service.Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	service.Pump();
	const auto first = store.ReadToken();
	CHECK( first.has_value() );

	// A day and a bit later, the daily check-in runs by itself and the new
	// token replaces the old.
	clock.now += 86400 + 120;
	service.Pump();
	CHECK( rig.server->requests.size() == 2 );
	if( rig.server->requests.size() == 2 )
	{
		CHECK( rig.server->requests[ 1 ].path == licence::PATH_HEARTBEAT );
		CHECK( StringField( rig.server->requests[ 1 ].fields, "machine" ) == rig.fingerprint );
		CHECK( StringField( rig.server->requests[ 1 ].fields, "product" ) == "datamosh" );
	}
	const auto second = store.ReadToken();
	CHECK( second.has_value() && second != first );
	CHECK( service.CurrentStatus() == Status::Active );

	// Offline for the next one: the cached token stays the answer and nothing
	// on screen changes.
	online = false;
	clock.now += 86400 + 120;
	service.Pump();
	CHECK( store.ReadToken() == second );
	CHECK( service.CurrentStatus() == Status::Active );
	CHECK( service.CurrentGate().restriction == Restriction::None );
	CHECK( service.Label() == "Licence: active" );

	// Offline long enough for the lease to lapse: a note, not a restriction.
	clock.now += 40 * 86400;
	service.Pump();
	CHECK( service.CurrentStatus() == Status::CheckInRequired );
	CHECK( service.CurrentGate().restriction == Restriction::None );
	CHECK( service.Label() == "Licence: active; check-in due" );
}

TEST( ARevokedLicenceDropsItsTokenAndKeepsItsKey )
{
	// A full refund revokes the licence, and the refund policy says that ends
	// it. So the heartbeat's "revoked" is honoured: the token goes, the key
	// stays so a reinstatement can come back through a later check-in.
	TempFolder  folder;
	Clock       clock;
	bool        revoked = false;
	auto        honest  = HonestServer( clock );
	TestLicence rig( folder.Path(), Policy::Lock, clock, [ & ]( const FakeServer::Request& request ) {
		if( revoked && request.path == licence::PATH_HEARTBEAT )
			return Respond( 403, "{\"ok\":false,\"reason\":\"revoked\","
			                     "\"message\":\"This licence was refunded and has ended.\"}" );
		return honest( request );
	} );
	licence::Service& service = *rig.service;
	licence::Store    store( folder.Path() );

	service.Pump();
	service.Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	service.Pump();
	CHECK( service.CurrentStatus() == Status::Active );
	licence::GateLatch running;
	CHECK( running.Update( service.CurrentGate() ).restriction == Restriction::None );

	// Refunded. The next daily check-in hears about it.
	revoked = true;
	clock.now += 86400 + 120;
	service.Pump();
	CHECK( rig.server->requests.size() == 2 );
	CHECK( !store.ReadToken().has_value() );
	CHECK( store.ReadKey() == std::optional< std::string >( "LT-DATA-K7M2-9PQR-4XTC" ) );
	CHECK( service.CurrentStatus() == Status::Invalid );
	CHECK( service.CurrentGate().restriction == Restriction::Lock );
	CHECK( service.Label() == "Licence: locked, revoked" );
	const std::string readme = ReadFile( ( folder.Path() / "README.txt" ).string() );
	CHECK( readme.find( "This licence was revoked by the licence service: This licence was refunded and has "
	                    "ended." ) != std::string::npos );

	// An instance already running keeps its gate; a new one is locked.
	CHECK( running.Update( service.CurrentGate() ).restriction == Restriction::None );
	licence::GateLatch fresh;
	CHECK( fresh.Update( service.CurrentGate() ).restriction == Restriction::Lock );

	// Reinstated: the key is still here, so the next check-in brings it back.
	revoked = false;
	clock.now += 86400 + 120;
	service.Pump();
	CHECK( rig.server->requests.size() == 3 );
	if( rig.server->requests.size() == 3 )
		CHECK( rig.server->requests[ 2 ].path == licence::PATH_HEARTBEAT );
	CHECK( store.ReadToken().has_value() );
	CHECK( !store.ReadRevoked().has_value() );
	CHECK( service.CurrentStatus() == Status::Active );
	CHECK( service.Label() == "Licence: active" );
	CHECK( ReadFile( ( folder.Path() / "README.txt" ).string() ).find( "revoked" ) == std::string::npos );
}

TEST( OnlyARevocationDropsTheToken )
{
	// Every other refusal, and every failure to reach the service, leaves the
	// cached token deciding. A check-in mid-show must change nothing.
	TempFolder  folder;
	Clock       clock;
	int         answer = -1;
	auto        honest = HonestServer( clock );
	const std::vector< licence::HttpResponse > refusals = {
		Respond( 403, "{\"ok\":false,\"reason\":\"not_activated\",\"message\":\"Not activated here.\"}" ),
		Respond( 403, "{\"ok\":false,\"reason\":\"expired\",\"message\":\"Expired.\"}" ),
		Respond( 403, "{\"ok\":false,\"message\":\"No reason given.\"}" ),
		Respond( 500, "{\"ok\":false,\"reason\":\"server_error\",\"message\":\"Oops.\"}" ),
		Respond( 502, "<html>Bad gateway</html>" ),
		Unreachable(),
	};
	TestLicence rig( folder.Path(), Policy::Lock, clock, [ & ]( const FakeServer::Request& request ) {
		if( answer >= 0 && request.path == licence::PATH_HEARTBEAT )
			return refusals[ static_cast< size_t >( answer ) ];
		return honest( request );
	} );
	licence::Service& service = *rig.service;
	licence::Store    store( folder.Path() );

	service.Pump();
	service.Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	service.Pump();
	const auto token = store.ReadToken();
	CHECK( token.has_value() );

	for( answer = 0; answer < static_cast< int >( refusals.size() ); ++answer )
	{
		// An unreachable check-in retries hourly; the rest wait a day.
		clock.now += 86400 + 120;
		const size_t before = rig.server->requests.size();
		service.Pump();
		CHECK( rig.server->requests.size() == before + 1 );
		CHECK( store.ReadToken() == token );
		CHECK( !store.ReadRevoked().has_value() );
		CHECK( service.CurrentGate().restriction == Restriction::None );
	}
}

TEST( AnOfflineTokenIsTakenFromTheFieldOrTheFolder )
{
	Clock     clock;
	TokenSpec spec;
	spec.exp        = clock.now + 30 * 86400;
	spec.maintUntil = clock.now + 365 * 86400;

	{
		TempFolder  folder;
		TestLicence rig( folder.Path(), Policy::Watermark, clock );
		spec.machine = rig.machineHash;
		rig.service->Pump();
		rig.service->Submit( MintToken( spec ) );
		rig.service->Pump();
		CHECK( rig.server->requests.empty() );
		CHECK( rig.service->CurrentStatus() == Status::Active );
		// The key comes from the token, so a later check-in has one to send.
		CHECK( licence::Store( folder.Path() ).ReadKey() == std::optional< std::string >( spec.key ) );
	}
	{
		// Saved into the folder by hand: imported, then removed.
		TempFolder  folder;
		TestLicence rig( folder.Path(), Policy::Watermark, clock );
		spec.machine = rig.machineHash;
		rig.service->Pump();
		std::ofstream( folder.Path() / licence::Store::DROP_FILE ) << MintToken( spec ) << "\n";
		rig.service->Pump();
		CHECK( rig.service->CurrentStatus() == Status::Active );
		CHECK( !std::filesystem::exists( folder.Path() / licence::Store::DROP_FILE ) );
	}
	{
		// A token made for another machine's request code is refused, and the
		// file is left where it is with the reason in the name.
		TempFolder  folder;
		TestLicence rig( folder.Path(), Policy::Watermark, clock );
		spec.machine = letissier::machine_hash( "SOME-OTHER-MAC" );
		rig.service->Pump();
		std::ofstream( folder.Path() / licence::Store::DROP_FILE ) << MintToken( spec );
		rig.service->Pump();
		CHECK( rig.service->CurrentStatus() == Status::Invalid );
		CHECK( !licence::Store( folder.Path() ).ReadToken().has_value() );
		CHECK( std::filesystem::exists( folder.Path() / licence::Store::DROP_FILE ) );
		CHECK( rig.service->Label().find( "another computer" ) != std::string::npos );
	}
}

TEST( ATrialStartsFromAnEmailAddress )
{
	TempFolder  folder;
	Clock       clock;
	TestLicence rig( folder.Path(), Policy::Watermark, clock, HonestServer( clock, "trial", 14 * 86400 ) );
	rig.service->Pump();
	rig.service->Submit( "vj@example.com" );
	rig.service->Pump();

	CHECK( rig.server->requests.size() == 1 );
	if( !rig.server->requests.empty() )
	{
		const auto& sent = rig.server->requests[ 0 ];
		CHECK( sent.path == licence::PATH_TRIAL );
		CHECK( StringField( sent.fields, "email" ) == "vj@example.com" );
		CHECK( StringField( sent.fields, "product" ) == "datamosh" );
		CHECK( StringField( sent.fields, "machine" ) == rig.fingerprint );
	}
	CHECK( rig.service->CurrentStatus() == Status::Active );
	CHECK( rig.service->Label() == "Licence: trial, 14 days left" );
	CHECK( licence::Store( folder.Path() ).ReadKey() == std::optional< std::string >( "LT-DATA-TR1A-L000-0000" ) );

	// Fourteen days on, over.
	clock.now += 14 * 86400 + 1;
	rig.service->Pump();
	CHECK( rig.service->CurrentStatus() == Status::Expired );
	CHECK( rig.service->CurrentGate().mark == Mark::TrialEnded );
	CHECK( rig.service->Label() == "Licence: trial ended" );
}

TEST( BothPluginBinariesSeeOneLicence )
{
	// The effect and the mixer are separate binaries with separate statics.
	// What they share is the folder: one activates, the other notices.
	TempFolder  folder;
	Clock       clock;
	TestLicence effect( folder.Path(), Policy::Watermark, clock, HonestServer( clock ) );
	TestLicence mixer( folder.Path(), Policy::Watermark, clock, HonestServer( clock ) );
	effect.service->Pump();
	mixer.service->Pump();
	CHECK( mixer.service->CurrentGate().restriction == Restriction::Watermark );

	effect.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	effect.service->Pump();
	mixer.service->Pump();
	CHECK( mixer.service->CurrentStatus() == Status::Active );
	CHECK( mixer.service->CurrentGate().restriction == Restriction::None );
	CHECK( mixer.server->requests.empty() );

	// Released from the mixer, the effect's worker sees that too.
	mixer.service->Submit( "deactivate" );
	mixer.service->Pump();
	effect.service->Pump();
	CHECK( effect.service->CurrentStatus() == Status::Invalid );
	CHECK( mixer.server->requests.size() == 1 );
	if( !mixer.server->requests.empty() )
		CHECK( mixer.server->requests[ 0 ].path == licence::PATH_DEACTIVATE );
}

TEST( DeactivatingForgetsLocallyEvenOffline )
{
	TempFolder  folder;
	Clock       clock;
	bool        online = true;
	auto        honest = HonestServer( clock );
	TestLicence rig( folder.Path(), Policy::Watermark, clock, [ & ]( const FakeServer::Request& request ) {
		return online ? honest( request ) : Unreachable();
	} );
	rig.service->Pump();
	rig.service->Submit( "LT-DATA-K7M2-9PQR-4XTC" );
	rig.service->Pump();
	CHECK( rig.service->CurrentStatus() == Status::Active );

	online = false;
	rig.service->Submit( "deactivate" );
	rig.service->Pump();
	licence::Store store( folder.Path() );
	CHECK( !store.ReadToken().has_value() );
	CHECK( !store.ReadKey().has_value() );
	CHECK( rig.service->CurrentStatus() == Status::Invalid );
}

TEST( ABuildThatCannotVerifyRestrictsNothing )
{
	for( const char* key : { "", "REPLACE_WITH_YOUR_PUBLIC_KEY_HEX" } )
	{
		TempFolder  folder;
		Clock       clock;
		TestLicence rig( folder.Path(), Policy::Lock, clock, nullptr, "TEST-MACHINE-0001", key );
		rig.service->Pump();
		CHECK( rig.service->CurrentGate().decided );
		CHECK( rig.service->CurrentGate().restriction == Restriction::None );
		CHECK( rig.service->Label() == "Licence: not checked by this build" );
	}
	{
		// Nor does a machine whose id cannot be read.
		TempFolder  folder;
		Clock       clock;
		TestLicence rig( folder.Path(), Policy::Lock, clock, nullptr, "" );
		rig.service->Pump();
		CHECK( rig.service->CurrentGate().restriction == Restriction::None );
	}
}

}  // namespace datamosh::test
