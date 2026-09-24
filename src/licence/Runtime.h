#pragma once

// The one licence state per plugin binary, and the only licence header the
// plugins include.
//
// Every instance of the effect shares it, and so does every instance of the
// mixer — the two are separate binaries with separate statics, but they read
// and write the same per-user folder, and each one's worker notices the other's
// writes within a few seconds (see Store.h).
//
// Nothing here does I/O on the calling thread. Start() creates a thread;
// CurrentGate() is one atomic load; Submit() queues a string.

#include "Licence.h"

#include <Breadcrumb.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace datamosh::licence {

class Service;

/// Starts the worker if it is not already running. Called from InitGL — a real
/// instance about to render — and never from a constructor, because FFGL builds
/// a prototype instance just to read parameter names while the host scans its
/// plugin folders.
void Start();

/// The gate the render thread applies. Lock-free.
Gate CurrentGate();

/// Moves whenever the label does.
std::uint32_t LabelGeneration();

/// "Licence: active", "Licence: trial, 9 days left", "Licence: no seats free"...
std::string CurrentLabel();

/// Hands what was typed into the Licence field to the worker.
void Submit( const std::string& typed );

/// Names this binary ("Datamosh", "DatamoshTransplant") for its crash marker
/// and reports. Call before Start(); the first call wins. No I/O: the worker
/// creates the marker file (CrashMarks.h) when it starts, so the render
/// thread never waits on the disk for it.
void NameBinary( const char* binary );

/// True once the worker has mapped this process's crash marker. One atomic
/// load; an instance claims its breadcrumb slot on the first frame after.
bool CrashMarkerOpen();

/// A breadcrumb slot for one instance, or null when there is no marker yet
/// or every slot is taken. Lock-free.
BreadcrumbSlot* ClaimBreadcrumb();
void            ReleaseBreadcrumb( BreadcrumbSlot* slot );
/// Records the host's name and version, for the marker (now, or when the
/// worker maps it).
void            NoteHost( const char* name, const char* version );

/// An exception caught at the FFGL boundary, for a crash report (sent only
/// with the person's say-so). Lock-free from the caller's side; never throws.
void ReportCaught( const char* where, const char* what ) noexcept;

/// Opens the feedback page in the browser, from the worker thread.
void OpenFeedback();

namespace testing {

/// Routes every call above to `service` instead of the process-wide worker,
/// which is then never started. nullptr restores the default. The test suite
/// installs one before its first test so that no test touches the real
/// licence folder or the network.
void Install( Service* service );

/// Where crash markers go while a test service is installed. Empty — the
/// default — means none are written, so no test ever touches the real
/// licence folder.
void SetReportsFolder( const std::filesystem::path& folder );

/// Maps the crash marker under that folder, as the worker would on its own
/// thread when it starts. The test service has no worker, so a test that
/// wants a marker calls this itself.
void OpenCrashMarker();

}  // namespace testing

}  // namespace datamosh::licence
