#pragma once

// The four calls to the licence service, with the HTTP stack behind an
// interface so tests can stand in for the service without a network.
//
// Everything here runs on the licence worker thread. Nothing in this file is
// reachable from a render call.

#include <memory>
#include <string>

namespace datamosh::licence {

struct HttpResponse
{
	/// False when no HTTP response arrived at all: offline, DNS, TLS, timeout.
	/// That is never a licensing failure — the cached token stays the answer.
	bool        reached = false;
	int         status  = 0;
	std::string body;
	/// Why it was not reached, for the README. Never shown as a licence error.
	std::string error;
};

/// What a caller may ask of one request beyond its path and body.
struct PostOptions
{
	/// The whole request, resolve to last byte. The licence calls keep the
	/// transport's own (longer) default; crash and feedback reports ask for 8.
	int         timeoutSeconds = 0;
	/// Empty keeps the transport's default.
	std::string userAgent;
};

/// POSTs JSON to the licence service. One implementation per platform —
/// WinHTTP on Windows, NSURLSession on macOS — and a stub on Linux, where the
/// plugin is only ever built for the tests.
class Transport
{
public:
	virtual ~Transport() = default;
	/// `path` is below ServiceUrl(), e.g. "/api/licence/activate". Blocking,
	/// with its own timeout; called only from the worker thread.
	virtual HttpResponse Post( const std::string& path, const std::string& jsonBody ) = 0;
	/// The same, with a timeout and User-Agent of the caller's choosing. A
	/// transport that has no use for them — a test's stand-in — need not
	/// override this.
	virtual HttpResponse Post( const std::string& path, const std::string& jsonBody, const PostOptions& options )
	{
		( void )options;
		return Post( path, jsonBody );
	}
};

/// SERVICE_URL, or the LETISSIER_API environment variable when it is set to
/// an http(s) URL — the one override every letissier.ie client shares, for
/// pointing a build at a test deployment. Tokens are still verified against
/// the compiled-in key, so pointing it elsewhere cannot license anything.
std::string ServiceUrl();

/// A service base URL, split for transports that want the parts (WinHTTP).
struct ServiceAddress
{
	bool        valid  = false;
	bool        secure = true;
	std::string host;
	int         port   = 443;
	/// Any path the base carries, without a trailing slash; usually empty.
	std::string basePath;
};
ServiceAddress ParseServiceUrl( const std::string& url );

/// The platform's transport, or a stub that never reaches anything.
std::unique_ptr< Transport > MakePlatformTransport();

/// A service reply, normalised. Every field the brief's response shapes carry.
struct Reply
{
	bool        reached = false;  ///< an HTTP response arrived
	bool        ok      = false;  ///< and it said {"ok": true}
	int         http    = 0;
	std::string token;
	std::string machine;          ///< the hash the service recorded
	std::string key;              ///< trial replies carry the new key
	std::string product;
	std::string edition;
	std::string checkInBy;
	std::string maintenanceUntil;
	std::string expiresAt;
	std::string reason;           ///< on failure: bad_request, no_seats, ...
	std::string message;          ///< on failure: the service's own words, for the operator
	std::string transportError;   ///< when not reached
};

Reply ParseReply( const HttpResponse& response );

// The request bodies. `fingerprint` is the RAW platform id: the service hashes
// what it receives, so hashing it here would bind every token to
// sha256(sha256(id)) and every later check would say "another computer".

std::string ActivateBody( const std::string& key, const std::string& fingerprint, const std::string& label,
                          const std::string& product );
std::string HeartbeatBody( const std::string& key, const std::string& fingerprint, const std::string& product );
std::string DeactivateBody( const std::string& key, const std::string& fingerprint );
std::string TrialBody( const std::string& product, const std::string& email, const std::string& fingerprint );

inline constexpr const char* PATH_ACTIVATE   = "/api/licence/activate";
inline constexpr const char* PATH_HEARTBEAT  = "/api/licence/heartbeat";
inline constexpr const char* PATH_DEACTIVATE = "/api/licence/deactivate";
inline constexpr const char* PATH_TRIAL      = "/api/licence/trial";

}  // namespace datamosh::licence
