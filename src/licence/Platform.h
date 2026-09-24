#pragma once

// One implementation per platform: PlatformWindows.cpp, PlatformApple.mm,
// PlatformLinux.cpp. Each also defines MakePlatformTransport (Wire.h).

#include "Service.h"

namespace datamosh::licence {

/// The machine id, the licence folder, the HTTP stack. Does I/O — called once,
/// on the worker thread.
Environment PlatformEnvironment();

/// The per-user licence folder alone (Store.h), without the rest of the
/// environment. Cheap enough for a host call that happens once per process:
/// no machine id, no network, no file is touched.
std::filesystem::path PlatformDirectory();

/// Keeps this plugin binary mapped for the rest of the process, so the worker
/// thread's code can never be unloaded from under it. See Runtime.cpp.
void PinThisModule();

}  // namespace datamosh::licence
