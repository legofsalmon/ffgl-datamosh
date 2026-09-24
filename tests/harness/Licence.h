#pragma once

// Stand-ins for the licence service and the per-user folder, so no test ever
// touches the real licence folder or the network.

#include <Json.h>
#include <Service.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace datamosh::test {

/// The licence state every test runs under unless it says otherwise: the Open
/// policy, decided, so nothing is marked and existing render tests see exactly
/// what they always saw. Installed by main() before the first test.
licence::Service& DefaultLicenceService();

/// Installs a service for one scope and puts the default back after.
struct ScopedLicence
{
	explicit ScopedLicence( licence::Service& service );
	~ScopedLicence();
	ScopedLicence( const ScopedLicence& )            = delete;
	ScopedLicence& operator=( const ScopedLicence& ) = delete;
};

/// A fresh, empty folder that is removed again when the test ends.
class TempFolder
{
public:
	TempFolder();
	~TempFolder();
	const std::filesystem::path& Path() const { return path; }

private:
	std::filesystem::path path;
};

/// A signing key the tests own, so they can mint Datamosh tokens — which the
/// service's vectors, all issued for `vizz`, cannot provide.
std::string TestPublicKeyHex();
std::string MintToken( const std::string& claimsJson );

struct TokenSpec
{
	std::string  product    = "datamosh";
	std::string  edition    = "standard";
	std::string  machine;          ///< the HASH, as the service writes it
	std::string  key        = "LT-DATA-K7M2-9PQR-4XTC";
	std::int64_t exp        = 0;
	std::int64_t maintUntil = 0;
	std::int64_t iat        = 0;
};
std::string MintToken( const TokenSpec& spec );

/// A test clock the Settings can read.
struct Clock
{
	std::int64_t now = 1'760'086'400;
};

/// Records every request and answers from a handler, as the service would.
class FakeServer : public licence::Transport
{
public:
	struct Request
	{
		std::string                                    path;
		std::string                                    body;
		std::map< std::string, licence::json::Value > fields;
	};

	using Handler = std::function< licence::HttpResponse( const Request& ) >;

	explicit FakeServer( Handler handler ) :
		handler( std::move( handler ) )
	{
	}

	licence::HttpResponse Post( const std::string& path, const std::string& jsonBody ) override;

	std::vector< Request > requests;

private:
	Handler handler;
};

/// A reply with the given status and body.
licence::HttpResponse Respond( int status, const std::string& body );
/// No reply at all: offline.
licence::HttpResponse Unreachable();

/// A service wired to a test folder, clock, fingerprint and (optionally) fake
/// server. `server` stays owned by the service; the returned pointer is for
/// inspecting requests.
struct TestLicence
{
	TestLicence( const std::filesystem::path& folder, licence::Policy policy, Clock& clock,
	             FakeServer::Handler handler = nullptr, std::string fingerprint = "TEST-MACHINE-0001",
	             std::string publicKey = TestPublicKeyHex(), std::string product = "datamosh" );

	std::unique_ptr< licence::Service > service;
	FakeServer*                        server = nullptr;
	std::string                        fingerprint;
	std::string                        machineHash;
};

}  // namespace datamosh::test
