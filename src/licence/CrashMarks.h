#pragma once

// The crash marker: one small memory-mapped file per plugin binary per host
// process, holding a breadcrumb slot for each instance (src/core/Breadcrumb.h).
//
//   <licence folder>/reports/running/<binary>-<pid>.mark
//
// What it can honestly tell, and what it cannot:
//
// - If Resolume dies while an instance is inside one of our host calls, the
//   slot still says inFrame = 1 and names the stage, because the mapping's
//   pages outlive the process. The next load finds a marker whose process is
//   gone and turns that slot into an "unclean-exit" report.
// - If Resolume dies anywhere else — another plugin, the host itself, a
//   force-quit — every slot says inFrame = 0, and the marker is deleted with
//   no report: that death was not ours to report.
// - It carries no stack trace. Getting one needs a signal handler or an
//   unhandled-exception filter, which inside a host would replace Resolume's
//   own for every plugin in the process, and is not a plugin's call to make.
// - A GPU driver that faults asynchronously, after our call has returned,
//   is not attributed to us; one that faults inside our call is, even though
//   the fault may be the driver's. The stage name is what separates them.
//
// The file is created once per process, by the licence worker thread when it
// starts — never on a host call. An instance claims a slot on its first frame
// after that, which is an atomic flag in memory; after that the only thing on
// a frame is the plain stores Breadcrumb makes. The price is that an
// instance's first moments, before the worker has mapped the file, go
// unrecorded.

#include <Breadcrumb.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace datamosh::licence::crash {

inline constexpr int           SLOTS  = 64;
inline constexpr std::uint32_t LAYOUT = 1;
inline constexpr char          MAGIC[ 8 ] = { 'D', 'M', 'C', 'R', 'U', 'M', 'B', '1' };

/// The file, exactly as mapped.
struct MarkFile
{
	char           magic[ 8 ];
	std::uint32_t  layout;
	std::uint32_t  pid;
	std::int64_t   startedAt;
	char           binary[ 32 ];
	char           version[ 24 ];
	char           host[ 64 ];  ///< Resolume's name and version, from SetHostInfo
	BreadcrumbSlot slots[ SLOTS ];
};

/// The folder markers live in, below a licence folder.
std::filesystem::path RunningFolder( const std::filesystem::path& reportsFolder );

std::uint32_t CurrentPid();
/// True when a process with this id exists (and so may still be writing its
/// marker). Errs towards alive: a marker that is kept a little longer costs
/// nothing, one that is reported while its process runs is a false crash.
bool ProcessAlive( std::uint32_t pid );

/// Maps this process's marker for `binary` under `reportsFolder`, once.
/// Later calls are no-ops. False if it could not be created, in which case
/// every instance simply runs without a breadcrumb.
bool Open( const std::filesystem::path& reportsFolder, const char* binary, const char* version );

/// True once Open has succeeded. One atomic load.
bool IsOpen();

/// A free slot for an instance, or null when there is no marker or all
/// slots are taken. Lock-free.
BreadcrumbSlot* Claim();
/// Gives a slot back. Safe with null.
void Release( BreadcrumbSlot* slot );

/// Records the host's name and version in the marker header.
void SetHost( const char* name, const char* version );

/// What one dead instance left behind.
struct Finding
{
	std::string   binary;
	std::string   version;
	std::string   host;
	std::string   stage;
	std::uint64_t frames = 0;
	std::uint32_t width  = 0;
	std::uint32_t height = 0;
	std::uint32_t pid    = 0;
	/// How many instances that process had open.
	int           instances = 0;
};

/// Reads every marker for `binary` whose process has gone, returns one
/// finding per slot that died inside a call, and deletes those markers.
/// Markers of live processes — this one included — are left alone.
std::vector< Finding > Harvest( const std::filesystem::path& reportsFolder, const std::string& binary,
                                const std::function< bool( std::uint32_t ) >& alive, std::uint32_t self );

namespace testing {

/// Unmaps and forgets this process's marker, so a test can open another.
void Close();

}  // namespace testing

}  // namespace datamosh::licence::crash
