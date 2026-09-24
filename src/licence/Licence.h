#pragma once

// The licence decision, as pure functions: no file, no network, no clock of
// its own, no threads. Everything here is a function of its arguments, which
// is what lets tests/test_licence.cpp drive it with the licence service's own
// signed vectors.
//
// The I/O lives elsewhere: Store (the per-user files), Wire (the four HTTP
// calls), Service (the background worker) and Runtime (the one process-wide
// instance both plugin types read). The render thread only ever sees a Gate,
// through one atomic load a frame.

#include "sdk/licence.hpp"

#include <cstdint>
#include <string>

namespace datamosh::licence {

using Status = letissier::Status;
using Claims = letissier::Claims;

/// What an unlicensed copy does.
enum class Policy
{
	/// Behaves exactly as an unlicensed build always has. The Licence field
	/// still reports "unlicensed" and still accepts a key or a trial.
	Open,
	/// Everything works; the output carries a "DATAMOSH · UNLICENSED" band.
	/// A valid trial or licence removes it.
	Watermark,
	/// "Trial, then lock": with no licence and no running trial, a NEW
	/// instance passes its input through untouched and does no datamoshing.
	/// An instance already rendering is never locked (see GateLatch).
	///
	/// An untouched passthrough is also exactly what a plugin that failed to
	/// load looks like — this codebase's first trap. So a locked instance says
	/// so where a dead one cannot: the Licence field's name reads
	/// "Licence: locked, ...", and it writes one line to the host log.
	Lock,
};

/// THE SWITCH. Lock is the owner's choice ("trial, then lock").
///
/// Changing it is this one line. Everything that depends on it goes through
/// RestrictionFor below, and the tests pin all three values, so whichever is
/// chosen has already been exercised.
inline constexpr Policy POLICY = Policy::Lock;

/// The product id the service knows this plugin by. Sent on activate and
/// heartbeat, and required of every token's `product` claim: the SDK does not
/// check it, and a Vizz licence must not license Datamosh.
inline constexpr const char* PRODUCT = "datamosh";
/// The product tag in the first group of a Datamosh key: LT-DATA-....
inline constexpr const char* KEY_TAG = "DATA";

inline constexpr const char* SERVICE_URL = "https://letissier.ie";
inline constexpr const char* ACCOUNT_URL = "https://letissier.ie/account";

/// What rendering does about a status.
enum class Restriction : int
{
	None      = 0,
	Watermark = 1,
	Lock      = 2,
};

/// Which words the band says.
enum class Mark : int
{
	None       = 0,
	Unlicensed = 1,
	TrialEnded = 2,
};

/// What the render thread reads: one packed atomic a frame.
struct Gate
{
	Restriction restriction = Restriction::None;
	Mark        mark        = Mark::None;
	/// False until the worker has made its first decision. Undecided renders
	/// unrestricted — the decision is a file read away, and failing toward the
	/// customer for those first milliseconds is the right side to fail on.
	bool        decided     = false;

	bool operator==( const Gate& other ) const
	{
		return restriction == other.restriction && mark == other.mark && decided == other.decided;
	}
	bool operator!=( const Gate& other ) const { return !( *this == other ); }
};

/// Holds one plugin instance's gate for its lifetime, and only ever loosens it.
///
/// This is what "never interrupts a session already running" means for a
/// plugin: an instance is a session. A trial that ends, a token that lapses or
/// a licence released from the other plugin type mid-show must not put a band
/// across an output that is already on a screen, or lock a layer that is
/// already moshing. So a tightening only reaches instances created after it.
///
/// Loosening is immediate: typing a key into an unlicensed instance takes the
/// band off that instance on the next frame.
///
/// The first decided gate is adopted whatever it is. Before it arrives the
/// instance renders unrestricted, and that is a matter of the milliseconds a
/// worker takes to read one small file.
class GateLatch
{
public:
	Gate Update( const Gate& incoming )
	{
		if( !incoming.decided )
			return held;
		if( !held.decided || static_cast< int >( incoming.restriction ) < static_cast< int >( held.restriction ) )
			held = incoming;
		else if( incoming.restriction == held.restriction && incoming.restriction == Restriction::None )
			held = incoming;
		return held;
	}

	const Gate& Held() const { return held; }

private:
	Gate held;
};

/// The one place a status is given a cost.
///
/// Restricted: Invalid (no licence, or one that does not verify), Expired (a
/// trial that is over) and WrongMachine (a token copied from elsewhere).
///
/// Never restricted: UpdateRequired (a bought licence outside its update
/// window still owns this build) and CheckInRequired (a paying customer whose
/// lease lapsed, usually from being offline). Those show a note and nothing
/// else.
///
/// And a build that cannot verify anything restricts nothing, whatever the
/// policy — otherwise shipping without a key would mark every copy, silently,
/// in a way that looks exactly like a licensing decision.
Restriction RestrictionFor( Status status, Policy policy, bool publicKeyConfigured );

/// The words for a restricted status. None for the statuses that are never
/// restricted.
Mark MarkFor( Status status );

/// When this build was made, compiled in. Its entitlement is measured against
/// the licence's `maintUntil`, so it must be the build's date and nothing a
/// user can edit.
std::int64_t BuildDate();

/// The Ed25519 public key this build verifies with.
const std::string& PublicKey();

/// A 64-character hex string: something that could be an Ed25519 public key.
bool PublicKeyConfigured( const std::string& publicKeyHex );

struct Verdict
{
	Status status    = Status::Invalid;
	bool   hasClaims = false;  ///< only when the signature verified and the product matched
	Claims claims;
};

/// The whole offline decision. The vendor SDK's check plus the product check
/// it leaves to the caller.
///
/// `product` is an argument rather than PRODUCT so the service's vectors,
/// which are issued for `vizz`, can drive the tests.
Verdict Decide( const std::string& token, const std::string& fingerprint, std::int64_t buildDate,
                std::int64_t now, const std::string& publicKeyHex, const std::string& product );

/// What was typed into the Licence field.
enum class InputKind
{
	Empty,
	Key,           ///< an LT- key, normalised
	ForeignKey,    ///< an LT- key whose tag belongs to another product
	Token,         ///< a pasted offline-activation token
	Email,         ///< start a trial
	Folder,        ///< open the licence folder
	Deactivate,    ///< release this machine's seat and forget the licence
	CheckIn,       ///< check in now
	Feedback,      ///< open the feedback page in the browser
	SendReports,   ///< "send": the waiting crash report, this once
	DiscardReports,///< "discard": not this one
	ReportsOn,     ///< "always send" / "reports on": automatic crash reports
	ReportsOff,    ///< "reports off"
	Unrecognised,
};

struct Input
{
	InputKind   kind = InputKind::Empty;
	std::string value;    ///< normalised key, token or email
	std::string product;  ///< for ForeignKey: the product the tag belongs to
};

Input Classify( const std::string& typed );

/// Canonical key form, as the service normalises it: upper case, no spaces,
/// LT- prefix (also accepted as 1T- or missing), and I/L→1, O→0, U→V in the
/// body.
std::string NormaliseKey( const std::string& typed );

/// Short, human status for the Licence field's display name.
std::string DescribeStatus( Status status, bool hasToken, const Claims* claims, std::int64_t now,
                            bool publicKeyConfigured );

}  // namespace datamosh::licence
