#include "Licence.h"

#include <CrashMarks.h>
#include <Runtime.h>
#include <monocypher-ed25519.h>
#include <sdk/licence.hpp>

#include <atomic>
#include <cstring>

namespace datamosh::test {

namespace fs = std::filesystem;

namespace {

struct KeyPair
{
	uint8_t secret[ 64 ];
	uint8_t publicKey[ 32 ];
};

const KeyPair& TestKeys()
{
	static const KeyPair keys = [] {
		KeyPair pair{};
		uint8_t seed[ 32 ];
		for( int i = 0; i < 32; ++i )
			seed[ i ] = static_cast< uint8_t >( 0xD0 + i );
		// Monocypher wipes the seed it is given.
		crypto_ed25519_key_pair( pair.secret, pair.publicKey, seed );
		return pair;
	}();
	return keys;
}

std::string Base64Url( const uint8_t* data, size_t length )
{
	static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string        out;
	int                buffer = 0;
	int                bits   = 0;
	for( size_t i = 0; i < length; ++i )
	{
		buffer = ( buffer << 8 ) | data[ i ];
		bits += 8;
		while( bits >= 6 )
		{
			bits -= 6;
			out.push_back( alphabet[ ( buffer >> bits ) & 0x3F ] );
		}
	}
	if( bits > 0 )
		out.push_back( alphabet[ ( buffer << ( 6 - bits ) ) & 0x3F ] );
	return out;
}

}  // namespace

licence::Service& DefaultLicenceService()
{
	static licence::Service* service = [] {
		licence::Settings settings;
		settings.policy = licence::Policy::Open;
		settings.now    = [] { return std::int64_t( 1'760'086'400 ); };
		auto* created   = new licence::Service( settings, [] {
            licence::Environment environment;
            environment.fingerprint = "TEST-MACHINE-DEFAULT";
            // No folder: nothing is read or written, and under Open that
            // decides "unlicensed, unrestricted".
            return environment;
        } );
		created->Pump();
		return created;
	}();
	return *service;
}

ScopedLicence::ScopedLicence( licence::Service& service )
{
	licence::testing::Install( &service );
}

ScopedLicence::~ScopedLicence()
{
	licence::testing::Install( &DefaultLicenceService() );
}

TempFolder::TempFolder()
{
	static std::atomic< int > counter{ 0 };
	path = fs::temp_directory_path() /
	       ( "datamosh-licence-test-" + std::to_string( reinterpret_cast< std::uintptr_t >( this ) ) + "-" +
	         std::to_string( counter.fetch_add( 1 ) ) );
	std::error_code error;
	fs::remove_all( path, error );
	fs::create_directories( path, error );
}

TempFolder::~TempFolder()
{
	std::error_code error;
	fs::remove_all( path, error );
}

std::string TestPublicKeyHex()
{
	static const char* digits = "0123456789abcdef";
	std::string        hex;
	for( uint8_t byte : TestKeys().publicKey )
	{
		hex.push_back( digits[ byte >> 4 ] );
		hex.push_back( digits[ byte & 0x0F ] );
	}
	return hex;
}

std::string MintToken( const std::string& claimsJson )
{
	const std::string payload = Base64Url( reinterpret_cast< const uint8_t* >( claimsJson.data() ), claimsJson.size() );
	uint8_t           signature[ 64 ];
	// Over the ASCII of the payload segment, as the service signs.
	crypto_ed25519_sign( signature, TestKeys().secret, reinterpret_cast< const uint8_t* >( payload.data() ),
	                     payload.size() );
	return payload + "." + Base64Url( signature, sizeof( signature ) );
}

std::string MintToken( const TokenSpec& spec )
{
	using licence::json::Quote;
	const std::string claims =
		"{\"v\":1,\"key\":" + Quote( spec.key ) + ",\"product\":" + Quote( spec.product ) +
		",\"edition\":" + Quote( spec.edition ) +
		",\"customer\":\"11111111-2222-3333-4444-555555555555\",\"name\":\"Test Buyer\",\"seats\":1" +
		",\"maintUntil\":" + std::to_string( spec.maintUntil ) + ",\"exp\":" + std::to_string( spec.exp ) +
		",\"machine\":" + Quote( spec.machine ) + ",\"mode\":\"online\",\"iat\":" + std::to_string( spec.iat ) +
		",\"jti\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}";
	return MintToken( claims );
}

licence::HttpResponse FakeServer::Post( const std::string& path, const std::string& jsonBody )
{
	return Post( path, jsonBody, licence::PostOptions{} );
}

licence::HttpResponse FakeServer::Post( const std::string& path, const std::string& jsonBody,
                                        const licence::PostOptions& options )
{
	Request request{ path, jsonBody, licence::json::ParseObject( jsonBody ), options };
	requests.push_back( request );
	if( !handler )
		return Unreachable();
	return handler( request );
}

licence::HttpResponse Respond( int status, const std::string& body )
{
	licence::HttpResponse response;
	response.reached = true;
	response.status  = status;
	response.body    = body;
	return response;
}

licence::HttpResponse Unreachable()
{
	licence::HttpResponse response;
	response.error = "test: offline";
	return response;
}

TestLicence::TestLicence( const fs::path& folder, licence::Policy policy, Clock& clock, FakeServer::Handler handler,
                          std::string fingerprintIn, std::string publicKey, std::string product ) :
	fingerprint( std::move( fingerprintIn ) )
{
	machineHash = letissier::machine_hash( fingerprint );

	licence::Settings settings;
	settings.policy    = policy;
	settings.publicKey = std::move( publicKey );
	settings.product   = std::move( product );
	settings.buildDate = clock.now - 86400;
	settings.now       = [ &clock ] { return clock.now; };
	// Deterministic: the only live process is this one, so a marker with any
	// other id is a process that has gone.
	settings.processAlive = []( std::uint32_t pid ) { return pid == licence::crash::CurrentPid(); };

	auto transport = std::make_unique< FakeServer >( std::move( handler ) );
	server         = transport.get();

	auto shared = std::make_shared< std::unique_ptr< FakeServer > >( std::move( transport ) );
	auto urls = opened;
	service     = std::make_unique< licence::Service >(
        settings, [ folder, fingerprint = fingerprint, shared, urls ] {
            licence::Environment environment;
            environment.directory    = folder;
            environment.fingerprint  = fingerprint;
            environment.transport    = std::move( *shared );
            environment.machineLabel = "test rig";
            environment.osVersion    = "14.6";
            environment.openUrl      = [ urls ]( const std::string& url ) {
                urls->push_back( url );
                return true;
            };
            return environment;
        } );
}

}  // namespace datamosh::test
