#version 410 core

// Everything the warp decides that a diagnostic view has to be able to redraw.
//
// This file is not a shader on its own. MoshPipeline::CompileShaders prepends it
// verbatim to Mosh.glsl and to Composite.glsl, which is why it carries the
// #version line and they do not.
//
// It exists because the two passes have to agree about the gate. "Is the gate
// open anywhere?" can only be answered honestly by drawing the gate the mosh
// pass actually used, and that gate is a dozen lines of arithmetic over a
// sampled vector field. Two copies of those lines would agree the day they were
// written and drift afterwards, and a view that shows a different field than the
// warp used is worse than no view at all. One copy, called from both passes,
// makes that drift impossible.
//
// GLSL compile errors in either pass report line numbers in the CONCATENATED
// source, so they are offset by the length of this file.

// Mosh Amount maps to a hold time between these, log-spaced, so that equal
// slider travel is an equal ratio of hold — the taper every delay and reverb
// control uses, and the one a fader hand already knows.
const float HOLD_MIN_SECONDS = 0.05;
const float HOLD_MAX_SECONDS = 4.0;
// Linear fade-in over this much of the bottom of the travel, so the fader is
// gentle where a hand parks it and slams back to. A steep onset here would just
// move the cliff to the end of the fader an operator rests at.
const float HOLD_FADE_IN = 0.12;

float MoshHash( vec2 p, float seed )
{
	vec3 p3 = fract( vec3( p.x, p.y, p.x ) * 0.1031 + seed * 0.0973 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

/// The vector field as the warp sees it, in UV units, before gain and before
/// Direction.
///
/// Sampling the field at the block centre gives every pixel in a macroblock the
/// same vector, which is what produces hard-edged MPEG tearing. Sampling it
/// normally lets the hardware interpolate, which liquefies it. Softness
/// crossfades between the two, so one estimator covers both looks.
///
/// Corruption is applied in here rather than by the caller because it changes
/// the vector's LENGTH, and the gate is measured on that length. A caller that
/// applied it afterwards would gate on a vector that no longer exists.
vec2 MoshFlowAt( sampler2D flowField, vec2 texUV, vec2 flowRes,
                 float softness, float corruption, float corruptEpoch )
{
	vec2 blockUV    = ( floor( texUV * flowRes ) + 0.5 ) / flowRes;
	vec2 flowBlocky = texture( flowField, blockUV ).xy;
	vec2 flowSmooth = texture( flowField, texUV ).xy;
	vec2 flow       = mix( flowBlocky, flowSmooth, clamp( softness, 0.0, 1.0 ) );

	// Corrupt macroblocks: a fraction of blocks get a plausible but wrong
	// vector, mimicking a block whose data arrived damaged.
	if( corruption > 0.0 )
	{
		vec2  blockId = floor( texUV * flowRes );
		float roll    = MoshHash( blockId, corruptEpoch );
		if( roll < corruption )
		{
			float angle = MoshHash( blockId, corruptEpoch + 17.0 ) * 6.2831853;
			float scale = MoshHash( blockId, corruptEpoch + 41.0 ) * 2.0;
			flow = mat2( cos( angle ), -sin( angle ), sin( angle ), cos( angle ) ) * flow * scale;
		}
	}
	return flow;
}

/// Coarsen a motion vector onto a grid, the way a low-bitrate encoder has to.
/// The stepped movement that produces is a large part of what reads as
/// "compressed" rather than "warped".
///
/// This is applied to the DISPLACEMENT, after the gate has been measured on the
/// true motion — not to the stored field. Two reasons, both learned the hard
/// way. Quantising the stored field fed the quantiser its own output through
/// the temporal blend, which latched: whatever grid point it started on, it
/// stayed on, and starting at zero it stayed at zero. And quantising before the
/// gate meant any motion below half a grid step rounded to zero, which closed
/// the gate, which refreshed the block from the live frame — so the plugin
/// rendered a pixel-exact passthrough exactly where it was asked to damage the
/// image hardest. At the default block size the shipped Corrupt preset needed
/// 3 px/frame of motion before it did anything at all.
///
/// Downstream of the gate, neither is possible. A vector that rounds to zero
/// now means the block holds where it is without moving, which is what a
/// skipped block does in a real decoder, rather than refreshing from a
/// keyframe it never received.
vec2 MoshQuantise( vec2 flow, float quantise, float blockPixels, vec2 frameRes )
{
	if( quantise <= 0.0 )
		return flow;
	float stepPixels = mix( 0.5, max( 1.0, blockPixels * 0.5 ), clamp( quantise, 0.0, 1.0 ) );
	vec2  stepUV     = vec2( stepPixels ) / frameRes;
	return round( flow / stepUV ) * stepUV;
}

/// Still regions keep refreshing normally; moving ones carry old pixels forward.
/// Threshold at 0 moshes everything, which gives the full melt.
float MoshGate( float motionPixels, float thresholdPixels )
{
	return smoothstep( thresholdPixels, thresholdPixels + 0.5, motionPixels );
}

/// The frame step every spatial persistence term is defined against.
const float MOSH_REF_STEP = 1.0 / 60.0;

/// Normalise a spatial persistence term to that reference step.
///
/// `persistence` is not an output weight. The mosh pass writes its result into
/// the accumulation buffer, which is next frame's `AccumPrev`, so persistence
/// is the pole of a one-pole IIR and any raw multiplier on it is a RATE.
/// `MoshHold` and `MoshRetention` are built as exp2(-dt/T) precisely so that
/// applying them f times a second cancels dt. A raw spatial factor does not
/// cancel: survival over a wall-clock second is spatial^f, so a gate of 0.5 is
/// 0.5^30 at 30fps and 0.5^60 at 60 — two very different pictures from one
/// slider position. That is the class of bug 0.2.0 existed to remove, and the
/// gate was the one term still carrying it, because a raw multiply is
/// invisible next to two that look just like it.
///
/// Raising it to dt/ref fixes the units. Endpoints are exact, and at 60fps the
/// exponent is 1 and this is the identity — which is why it changes nothing
/// already measured. The saving grace until now was that smoothstep over half
/// a pixel of motion is a hairline: intermediate gate values are rare, so the
/// error was real but hard to see. Anything with a gradient across it would
/// not be so lucky, which is why this lands before the terms that have one.
float MoshNormaliseSpatial( float spatial, float deltaTime )
{
	if( spatial >= 0.999 )
		return 1.0;
	if( spatial <= 0.0 )
		return 0.0;
	return pow( spatial, deltaTime / MOSH_REF_STEP );
}

/// Mosh Amount as a hold TIME on a log scale, not a per-frame retention
/// fraction. A raw fraction decays geometrically, so a linear slider spent most
/// of its travel inside the first few frames of hold and crushed the entire
/// usable range into its top tenth — and, being per-frame, it meant something
/// different at every frame rate. Endpoints are preserved: 0 is still an exact
/// passthrough and 1 is still an exact never-refresh.
float MoshHold( float moshLevel, float deltaTime )
{
	if( moshLevel >= 0.995 )
		return 1.0;
	float hold = HOLD_MIN_SECONDS * pow( HOLD_MAX_SECONDS / HOLD_MIN_SECONDS, moshLevel );
	return exp2( -deltaTime / hold ) * clamp( moshLevel / HOLD_FADE_IN, 0.0, 1.0 );
}

/// Decay expressed as a half-life rather than a per-frame fraction, so the same
/// setting bleeds the live image back at the same rate whether the host is
/// running at 30 or 60 or an uneven frame rate. A flat multiplier would decay
/// twice as fast at double the frame rate, which makes the control mean
/// something different on every machine it runs on.
///
/// Geometric, not linear, between the endpoints: equal travel is an equal ratio
/// of half-life. Linear put 0.5 at a 4s half-life — longer than any burst — so
/// nothing happened for four fifths of the slider and then it collapsed. Now 0.5
/// is 0.63s.
float MoshRetention( float decay, float deltaTime )
{
	if( decay <= 0.0 )
		return 1.0;
	float halfLife = 8.0 * pow( 0.00625, clamp( decay, 0.0, 1.0 ) );
	return exp2( -deltaTime / halfLife );
}
