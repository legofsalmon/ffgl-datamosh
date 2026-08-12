#pragma once

namespace datamosh {

/// What decides when pixels stop refreshing from the live input, i.e. when a
/// codec would have dropped a keyframe.
enum class AutoMode : int
{
	Manual = 0,  ///< Only the Trigger button and the Amount dial.
	OnCut  = 1,  ///< Detect hard cuts in the incoming video and melt through them.
	OnBeat = 2,  ///< Fire on Resolume's clock, every `beatDivisor` beats.
};

/// Trades motion-search cost against fidelity. Everything else being equal this
/// is the dial to reach for when a show needs headroom.
enum class Quality : int
{
	Low    = 0,  ///< 2 pyramid levels.
	Medium = 1,  ///< 3 levels.
	High   = 2,  ///< 4 levels.
	Ultra  = 3,  ///< 5 levels plus a second refinement at full resolution.
};

/// Which input drives motion in the two-input mixer build.
enum class MotionSource : int
{
	FromA = 0,  ///< Motion from input A, pixels from input B.
	FromB = 1,  ///< Motion from input B, pixels from input A.
};

/// Everything the pipeline needs for one frame. Plain data so the effect, the
/// mixer and the test harness all drive the pipeline identically.
struct MoshParams
{
	// --- Mosh -------------------------------------------------------------
	/// Master gate. 0 refreshes every pixel from the input (bypass), 1 never
	/// refreshes, so the image is carried entirely by motion compensation.
	float    moshAmount = 0.0f;
	/// One-shot: start a timed mosh burst this frame.
	bool     trigger    = false;
	/// One-shot: force a keyframe, snapping back to the clean input.
	bool     reset      = false;
	AutoMode autoMode   = AutoMode::Manual;
	/// Cut-detection sensitivity, 0..1. Higher fires on smaller changes.
	float    sensitivity = 0.5f;
	/// Seconds a triggered burst lasts before decaying back.
	float    duration    = 1.0f;

	// --- Motion -----------------------------------------------------------
	/// Multiplier on the estimated vectors. >1 exaggerates the smear.
	float motionGain      = 1.0f;
	/// 0..1 blend toward holding the vector field instead of re-estimating it.
	/// At 1 this is P-frame duplication: motion keeps applying and blooms.
	float motionFreeze    = 0.0f;
	/// 0..1 temporal blend of the vector field. Longer trails, more inertia.
	float motionSmoothing = 0.3f;
	/// Minimum motion, in pixels, before a region is treated as moving.
	float motionThreshold = 0.15f;
	/// Macroblock edge in pixels. 4/8/16/32; 16 is the classic MPEG size.
	int   blockSize       = 16;
	/// 0 = hard macroblock edges, 1 = smooth per-pixel liquid flow.
	float softness        = 0.0f;
	/// 0..1 snap of the warp offset to whole pixels. Keeps the accumulation
	/// buffer crisp; without it repeated bilinear resampling turns it to mush.
	float pelSnap         = 1.0f;
	bool  invertDirection = false;

	// --- Damage -----------------------------------------------------------
	/// Bleeds the live image back in even at full mosh. 0 holds forever.
	float decay       = 0.0f;
	/// 0..1 chance of a block holding or scrambling its vector.
	float corruption  = 0.0f;
	/// Per-channel offset along the motion vector.
	float chromaDrift = 0.0f;
	/// Wet/dry against the untouched input.
	float mix         = 1.0f;

	// --- Sync -------------------------------------------------------------
	/// 0..1 audio level, from the host's FFT.
	float audioLevel  = 0.0f;
	/// How much `audioLevel` adds to the mosh amount.
	float audioAmount = 0.0f;
	/// Host tempo and position within the bar, straight from SetBeatInfo.
	float bpm         = 120.0f;
	float barPhase    = 0.0f;
	/// Fire every N beats in AutoMode::OnBeat.
	int   beatDivisor = 4;

	// --- Rendering --------------------------------------------------------
	Quality      quality      = Quality::High;
	MotionSource motionSource = MotionSource::FromA;

	/// Seconds since the last state advance. The pipeline uses this rather than
	/// a frame count so behaviour is identical at any host frame rate.
	float deltaTime = 1.0f / 60.0f;
	/// Monotonic frame counter, used only to decorrelate the corruption noise.
	int   frame     = 0;
};

/// Pyramid depth for a quality setting.
inline int PyramidLevelsFor( Quality quality )
{
	switch( quality )
	{
	case Quality::Low:    return 2;
	case Quality::Medium: return 3;
	case Quality::High:   return 4;
	case Quality::Ultra:  return 5;
	}
	return 4;
}

/// Whether to spend a second refinement iteration at the finest level.
inline bool ExtraRefinementFor( Quality quality )
{
	return quality == Quality::Ultra;
}

}  // namespace datamosh
