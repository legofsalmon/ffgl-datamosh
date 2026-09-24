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

#include <cstdint>
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

namespace testing {

/// Routes every call above to `service` instead of the process-wide worker,
/// which is then never started. nullptr restores the default. The test suite
/// installs one before its first test so that no test touches the real
/// licence folder or the network.
void Install( Service* service );

}  // namespace testing

}  // namespace datamosh::licence
