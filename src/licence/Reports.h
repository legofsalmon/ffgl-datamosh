#pragma once

// Crash reports and feedback, to letissier.ie and nowhere else.
//
// The contract is the site's (/api/reports/crash, /feedback); this is the
// plugin's half of it, fitted to what a plugin inside someone else's
// application can honestly do:
//
// - Crashes are detected on the NEXT load, from the marker a dead process left
//   behind (CrashMarks.h), plus exceptions caught at the FFGL boundary while
//   running. There is no signal handler and no stack trace; see CrashMarks.h
//   for why.
// - Nothing is sent unless the person has said so: either the setting "send
//   crash reports automatically" (off by default), or "send" typed in answer
//   to the one-time prompt the Licence field's name shows after a problem.
//   Both are typed into the Licence field, the one text input a plugin has.
// - Everything is scrubbed on this computer before it is written, queued as
//   JSON files (at most 20, oldest dropped), and sent from the licence worker
//   thread with an 8 second timeout. Never from a render call, never while a
//   composition is loading, never an error anyone sees.
// - Feedback has no form to live in: the plugin opens the site's feedback page
//   in the browser, with the product and version filled in.
//
// Everything that decides something is a plain function or takes its clock,
// folder and transport as arguments, so tests/test_reports.cpp can drive it
// without a network or the real licence folder.

#include "CrashMarks.h"
#include "Wire.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace datamosh::licence::reports {

inline constexpr const char* PRODUCT        = "datamosh";
inline constexpr const char* CRASH_PATH     = "/api/reports/crash";
inline constexpr const char* FEEDBACK_PAGE  = "/feedback";
inline constexpr size_t      QUEUE_LIMIT    = 20;
inline constexpr int         TIMEOUT_SECONDS = 8;
/// Queued reports wait this long after launch before going out, so sending
/// never competes with a composition loading.
inline constexpr std::int64_t SEND_DELAY    = 60;
/// Two presses of Send Feedback closer than this open one page, not two.
inline constexpr std::int64_t FEEDBACK_DEBOUNCE = 5;

// Field limits from the intake contract.
inline constexpr size_t MAX_VERSION    = 32;
inline constexpr size_t MAX_OS_VERSION = 32;
inline constexpr size_t MAX_ARCH       = 16;
inline constexpr size_t MAX_INSTALL    = 64;
inline constexpr size_t MAX_SUMMARY    = 300;
inline constexpr size_t MAX_DETAIL     = 32768;
inline constexpr size_t MAX_SIGNATURE  = 128;
inline constexpr size_t MAX_BODY       = 64 * 1024;

/// "macos", "windows" or "linux" — this build's platform, in the contract's words.
const char* OsName();
/// "arm64" or "x86_64" — this slice of the binary.
const char* Arch();

/// "Datamosh/1.0.0 (macos)"
std::string UserAgent( const std::string& version );

/// The browser page for feedback: `<base>/feedback?product=datamosh&version=<v>`.
std::string FeedbackUrl( const std::string& base, const std::string& version );

/// Removes what must never leave this computer from free text: the home
/// folder becomes `~`, any user name in a /Users/, /home/ or C:\Users\ path
/// becomes `<user>`, the user name on its own becomes `<user>`, and anything
/// after `?` in a URL is dropped.
std::string Scrub( const std::string& text, const std::string& home, const std::string& user );

/// At most `limit` bytes, never splitting a UTF-8 sequence.
std::string Truncate( const std::string& text, size_t limit );

/// A short, stable id for "the same problem": kind, binary and where.
std::string Signature( const std::string& kind, const std::string& binary, const std::string& where );

/// A random version-4 UUID. From std::random_device — never from a licence,
/// a hardware id, a MAC address, a host name or a user name.
std::string NewInstallId();
bool        ValidInstallId( const std::string& id );

/// One crash report, before it is written as JSON.
struct CrashReport
{
	std::string version;
	std::string os;
	std::string osVersion;
	std::string arch;
	std::string install;
	std::string kind;       ///< unclean-exit, exception, ...
	std::string summary;
	std::string detail;
	std::string signature;
	std::string occurredAt; ///< ISO 8601, or empty
};

/// The JSON body, with every field cut to the contract's limit and only the
/// contract's fields. `note` is never sent: the plugin has nowhere to type one.
std::string CrashPayload( const CrashReport& report );

/// What to do with a queued report after trying to send it.
enum class Outcome
{
	Sent,  ///< 202: stored. Delete it.
	Drop,  ///< 400 or 413: the service will never take it. Delete it.
	Keep,  ///< 429, 5xx, no answer: try again next launch.
};
Outcome Classify( const HttpResponse& response );

/// "2026-09-24T02:10:00Z"
std::string IsoTime( std::int64_t unixSeconds );

/// The report folder's queue of JSON files: `queue/` holds what the person
/// agreed to send, `pending/` what is waiting for their answer.
class Queue
{
public:
	explicit Queue( std::filesystem::path folder );

	const std::filesystem::path& Folder() const { return folder; }

	/// Writes one report; drops the oldest beyond QUEUE_LIMIT. False if it
	/// could not be written.
	bool Add( const std::string& json, const std::string& stem );
	/// Oldest first.
	std::vector< std::filesystem::path > Files() const;
	size_t Size() const { return Files().size(); }
	void   Clear() const;

private:
	std::filesystem::path folder;
};

/// The crash-report half of the licence worker. Owned by Service, pumped on
/// its thread; nothing here is called from a render call.
class Reporter
{
public:
	struct Config
	{
		/// `<licence folder>/reports`. Empty turns everything off.
		std::filesystem::path folder;
		/// Where the install id lives, beside the licence files.
		std::filesystem::path installIdFile;
		std::string           binary;
		std::string           version;
		std::string           osVersion;
		std::string           home;
		std::string           user;
		std::uint32_t         self = 0;
		std::function< bool( std::uint32_t ) > alive;
		std::function< std::int64_t() >        now;
		std::string           serviceBase;
	};

	Reporter( Config config, Transport* transport, std::function< bool( const std::string& ) > openUrl );

	/// Once, when the worker starts: collects what a dead process left behind,
	/// forgets any question from an earlier launch that was never answered
	/// (it was asked once), and schedules the queue to go out after SEND_DELAY.
	void Start();

	/// Sends the queue when it is due and there is a network. Worker thread.
	void Pump();

	/// An exception caught at the FFGL boundary. The same one is reported once
	/// per process.
	void Caught( const std::string& where, const std::string& what );

	// What the person typed into the Licence field.
	void Send();                     ///< "send": this time
	void Discard();                  ///< "discard": not this time
	void SetAutomatic( bool on );    ///< "reports on" / "reports off"
	bool OpenFeedback();             ///< "feedback", or the Send Feedback button

	bool Automatic() const;
	/// True while a report is waiting for the person's answer.
	bool Asking() const;
	/// True when what is waiting includes a death, not only caught errors.
	bool AskingAboutACrash() const;
	size_t Queued() const { return queue.Size(); }
	std::string Notice() const { return notice; }

	/// The README.txt section: the setting, what is waiting, what is sent and
	/// what never is.
	std::string Describe() const;

	const std::string& InstallId() const { return installId; }

private:
	bool Enabled() const { return !config.folder.empty(); }
	std::string Stem( const std::string& kind );
	void        Record( const CrashReport& report, const std::string& stem, bool crash );
	void        Promote();
	void        Flush();
	std::string Scrubbed( const std::string& text ) const;
	std::string DetailLines( const std::vector< std::pair< std::string, std::string > >& lines ) const;
	std::filesystem::path SettingsFile() const;

	Config                   config;
	Transport*               transport;
	std::function< bool( const std::string& ) > openUrl;

	Queue                    queue;
	Queue                    pending;
	std::string              installId;
	std::int64_t             sendAfter    = 0;
	bool                     flushBlocked = false;
	bool                     askedAboutCrash = false;
	std::int64_t             lastFeedback = -FEEDBACK_DEBOUNCE - 1;
	std::set< std::string >  seenThisProcess;
	int                      sequence = 0;
	std::string              notice;
};

}  // namespace datamosh::licence::reports
