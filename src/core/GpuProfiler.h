#pragma once

#include "GL.h"

#include <array>
#include <cstddef>

namespace datamosh {

/// The stages worth measuring separately when deciding where the frame time is
/// going. Ordered as they run.
enum class Pass : int
{
	Ingest = 0,
	Luma,
	SceneDiff,
	Control,
	MotionSearch,
	FlowPost,
	Mosh,
	Composite,
	Count
};

const char* PassName( Pass pass );

/// Per-pass GPU timing, off by default and free when off.
///
/// Motion search dominates the cost and scales with block size, resolution and
/// quality all at once, so tuning it by changing a setting and watching the
/// frame rate is guesswork. This attributes time to the pass that spent it.
///
/// Results are read three frames late through a ring of query objects. Asking
/// for a query's result in the frame that issued it forces the CPU to wait for
/// the GPU, which would make the profiler the slowest thing in the pipeline and
/// the numbers it reported meaningless.
class GpuProfiler
{
public:
	GpuProfiler() = default;
	~GpuProfiler();

	GpuProfiler( const GpuProfiler& )            = delete;
	GpuProfiler& operator=( const GpuProfiler& ) = delete;

	/// Allocates query objects. Safe to call when already enabled.
	bool Enable();
	void Release();

	bool IsEnabled() const { return enabled; }

	/// Starts timing a pass. Ends any pass still open.
	void Begin( Pass pass );
	/// Ends the currently open pass, if any.
	void End();

	/// Rotates the ring and collects whatever results are ready.
	void NextFrame();

	/// Most recent measurement for a pass, in milliseconds.
	float GetMilliseconds( Pass pass ) const;
	/// Sum across all passes, in milliseconds.
	float GetTotalMilliseconds() const;

private:
	static constexpr int PASS_COUNT  = static_cast< int >( Pass::Count );
	static constexpr int RING_LENGTH = 3;

	struct Slot
	{
		std::array< GLuint, PASS_COUNT > queries{};
		std::array< bool, PASS_COUNT >   issued{};
	};

	bool                                  enabled = false;
	int                                   writeIndex = 0;
	std::array< Slot, RING_LENGTH >       ring{};
	std::array< float, PASS_COUNT >       results{};
	int                                   openPass = -1;
};

/// Times a pass for the duration of a scope.
class ScopedPassTimer
{
public:
	ScopedPassTimer( GpuProfiler& profiler, Pass pass ) :
		profiler( profiler )
	{
		profiler.Begin( pass );
	}
	~ScopedPassTimer() { profiler.End(); }

	ScopedPassTimer( const ScopedPassTimer& )            = delete;
	ScopedPassTimer& operator=( const ScopedPassTimer& ) = delete;

private:
	GpuProfiler& profiler;
};

}  // namespace datamosh
