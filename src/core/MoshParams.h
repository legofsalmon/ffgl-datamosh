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
/// What the composite pass draws. Result is the effect; the others are
/// diagnostics that replay the decision the warp actually took.
enum class DebugView : int
{
	Result = 0,
	Motion,
	Gate
};

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
	/// Played like a key: the mosh is held for exactly as long as this is true,
	/// with no reference to `duration`. Releasing it falls through the same
	/// ramp a burst does. Unlike `trigger` this is a level, not an edge, so a
	/// dropped release leaves the effect on — `reset` clears it.
	bool     hold       = false;
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
	/// 0..1 temporal inertia of the vector field, as a hold time on a log
	/// scale rather than a raw per-frame blend. At 1 the field never updates:
	/// P-frame duplication, the bloom. This absorbed the old `motionFreeze`,
	/// which was the same operation applied a second time — two successive
	/// mixes toward the same target compose to one, 1-(1-S)(1-F), symmetric.
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
	/// Bleeds the live image back in even at full mosh, as a half-life. 0 holds
	/// forever; 1 is a fraction of a second.
	float decay       = 0.0f;
	/// 0..1 chance of a block holding or scrambling its vector.
	float corruption  = 0.0f;
	/// Per-channel offset along the motion vector.
	float chromaDrift = 0.0f;
	/// Wet/dry against the untouched input.
	float mix         = 1.0f;

	/// Frames of delay between estimating the motion field and applying it.
	///
	/// The estimator is accurate enough that displacing the previous frame by it
	/// reproduces the current frame — correct, and far too clean to read as a
	/// broken codec. Feeding the warp a field from several frames ago applies
	/// motion to content it does not belong to, which is what a decoder working
	/// from wrong reference frames actually does.
	int   motionLag   = 0;
	/// 0..1 chance of a block taking a neighbour's content instead of its own,
	/// the look of a block whose data never arrived.
	float blockRepeat = 0.0f;
	/// Snaps vectors to a coarse grid the way a low-bitrate encoder must,
	/// giving stepped chunky motion rather than smooth displacement.
	float motionQuantise = 0.0f;
	/// 0..1 how fast damage creeps outward from the blocks that seeded it, as a
	/// speed in frame heights per second on a log scale. 0 is off and the field
	/// stays exactly empty, which is what makes this not a breaking change.
	float spread         = 0.0f;

	/// What to draw. Result is the effect and is what every existing
	/// composition gets; the others are diagnostics.
	DebugView view       = DebugView::Result;

	// --- Mask -------------------------------------------------------------
	/// 0..1 depth of the luma mask on the persistence gate. The mask comes from
	/// the MOTION input's brightness — the clip's own in the effect, the other
	/// layer's in the mixer — so the layer that supplies the motion also paints
	/// where the mosh is allowed to stick. 0 leaves it inert.
	float maskAmount = 0.0f;
	/// Mosh the dark parts instead of the bright ones.
	bool  maskInvert = false;

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
	/// Monotonic frame counter.
	int   frame     = 0;
	/// Seed for the corruption and block-repeat hashes. Advances on eighth
	/// notes when the host runs a clock, every few frames otherwise — so a
	/// broken block stays broken for a musical duration instead of re-rolling
	/// every frame into 60Hz shimmer. Real damage persists; that is the point.
	int   corruptEpoch = 0;
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
