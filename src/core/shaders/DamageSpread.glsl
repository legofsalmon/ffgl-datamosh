#version 410 core

// Spreading damage: a per-block field saying how far the corruption has crept.
//
// Everything else this plugin does is affine — a per-pixel decision computed
// from this frame's numbers alone. This is the one pass with a spatial memory:
// a block that is not moving can still be moshing, because the block next door
// was, half a second ago. Corruption starts where motion is strongest and
// creeps outward; when the level falls the frontier — which is always the
// faintest part of the field — drops out first, so it retreats the way it came.
//
// TWO THINGS IN HERE ARE NOT NEGOTIABLE.
//
// 1. The spread is a DILATION (max over a ring of taps), not a blur. A mean
//    blur is diffusion: its front advances as the square root of elapsed time,
//    and where you judge the front to be depends on the level you read it at.
//    No choice of coefficient makes that frame-rate independent. A max
//    dilation is morphological — radii compose by addition — so the distance
//    covered depends only on wall-clock time.
//
// 2. Both rates are per SECOND. The radius is speed*DeltaTime and the
//    attenuation is a half-life. A fixed per-frame radius would move the front
//    twice as fast at 120fps as at 60, which is the whole reason 0.2.0 exists.

uniform sampler2D PrevDamage;  // last frame's field, this shader's own output
uniform sampler2D Flow;        // the lagged conditioned field PassMosh will use
uniform sampler2D State;       // 1x1 control state; .r is the mosh level

uniform vec2  FlowRes;
uniform vec2  FrameRes;
uniform float BlockPixels;      // macroblock edge, so one texel is one block
uniform float ThresholdPixels;  // identical to the value PassMosh gates on
uniform float Spread;           // 0..1, 0 is off and the field stays empty
uniform float DeltaTime;        // seconds since the last advance
uniform bool  HasHistory;       // false on the first frame and after a reset

// Spread maps to a propagation SPEED in frame heights per second, log-spaced,
// so equal slider travel is an equal ratio of speed — the same taper as Mosh
// Amount, Motion Smoothing and Decay. Expressed in frame heights rather than
// pixels or blocks so that changing Block Size or the composition resolution
// does not change how fast the damage visibly travels.
// Measured, not guessed. The reach that matters is where the field is still
// strong enough to open the gate — 1.51 * SPREAD_HALF_LIFE * speed — so reach
// is LINEAR in speed while the slider is logarithmic in it. The first range
// tried here, 0.15 to 1.5 heights/second against a 0.15s half-life, put the
// whole usable creep in the top of the travel:
//
//   Spread 0.25 -> 0 blocks   0.50 -> 1 block   0.75 -> 2   1.00 -> 5
//
// which is the fault 0.2.0 existed to remove, reintroduced in a new control.
// Widening the ratio and lengthening the half-life below spreads it out:
//
//   Spread 0.25 -> 4 blocks   0.50 -> 8         0.75 -> 16  1.00 -> 31
//
// as a fraction of frame width at 16:9 and Block Size 16: 10%, 20%, 39%, 77%.
const float SPREAD_MIN_HEIGHTS = 0.12;  // a halo a couple of blocks deep
const float SPREAD_MAX_HEIGHTS = 1.8;   // most of the frame

// How long damage survives once nothing is re-seeding it. Fixed rather than
// exposed: with the speed above it sets the reach, so one control governs both
// how fast the creep travels and how far it gets, which is what makes it one
// gesture rather than two knobs that have to agree.
//
// 0.15s was too short to reach anywhere before the field faded — it is the
// multiplier on the whole reach, so it was throttling every Spread setting at
// once. Half a second still retreats promptly when the level comes down, which
// is the other half of what this constant controls.
const float SPREAD_HALF_LIFE = 0.5;

// Fades the seed in over the bottom of the travel, so leaving Spread at 0 is an
// exact bypass without the first touch of the fader being a step. Same device
// as HOLD_FADE_IN in MoshCommon.glsl.
const float SPREAD_ARM = 0.02;

// Damage below this is extinguished rather than left to decay asymptotically.
// Half-float has resolution far below anything visible, and without a floor a
// ghost field lingers in the buffer for minutes after the gesture ended.
const float SPREAD_FLOOR = 0.002;

// Ceiling on how far the front may advance in one pass. Past about this the
// taps stop overlapping and the dilation starts leaving holes. The cost is that
// at a very fine Block Size on a very large frame the top of Spread saturates
// and runs slower than nominal — at 1080p, Block Size 4, Spread 1.0 and 60fps
// it wants 8.1 texels against the 4 allowed, so about half speed. At the
// default Block Size 16 it wants 2.0 and the cap is never near. The alternative is a
// variable iteration count, which makes the pass most expensive exactly when
// the machine is already dropping frames.
const float MAX_STEP_TEXELS = 4.0;

// Eight directions at unit Euclidean distance. Diagonals at 1/sqrt(2) rather
// than 1, or the front would be a square travelling 1.41x faster on the
// diagonal and the speed above would mean two different things.
const float DIAG = 0.70710678;
const vec2  SPREAD_DIRS[ 8 ] = vec2[ 8 ](
	vec2(  1.0,  0.0 ), vec2( -1.0,  0.0 ), vec2(  0.0,  1.0 ), vec2(  0.0, -1.0 ),
	vec2( DIAG, DIAG ), vec2( -DIAG, DIAG ), vec2( DIAG, -DIAG ), vec2( -DIAG, -DIAG ) );

in vec2 uv;
out vec4 fragColor;

/// Rejects non-finite values before they can enter the feedback buffer.
/// This field is read back into itself every frame: one NaN would spread
/// through the dilation to the whole frame and only a host restart would clear
/// it. Same discipline, and the same reason, as FlowPost.glsl's Sanitise.
float Sanitise( float v )
{
	return ( isnan( v ) || isinf( v ) ) ? 0.0 : clamp( v, 0.0, 1.0 );
}

void main()
{
	float spread = clamp( Spread, 0.0, 1.0 );

	// --- the seed: today's global answer, evaluated per block ---------------
	//
	// moshLevel is the same one-texel level the mosh pass reads, and the gate is
	// the same smoothstep on the same vectors, so a block that was already
	// moshing is seeded at full strength and nothing about it changes.
	float moshLevel = Sanitise( texture( State, vec2( 0.5 ) ).r );

	// This target is exactly Flow's resolution, so the fragment centre lands on
	// the matching texel centre and the bilinear fetch returns it unfiltered.
	vec2 flow = texture( Flow, uv ).xy;
	if( any( isnan( flow ) ) || any( isinf( flow ) ) )
		flow = vec2( 0.0 );

	float motionPixels = length( flow * FrameRes );
	float gate         = smoothstep( ThresholdPixels, ThresholdPixels + 0.5, motionPixels );

	// moshLevel scales the seed, so the fader controls how far the creep gets:
	// the front dies where moshLevel * attenuation falls below visibility. It is
	// also what makes the release retreat — as the level falls the seed weakens,
	// nothing renews the frontier, and the faintest blocks go out first.
	float seeded = moshLevel * gate * clamp( spread / SPREAD_ARM, 0.0, 1.0 );

	// --- the spread: a dilation of last frame's field -----------------------
	float propagated = 0.0;
	if( HasHistory && spread > 0.0 )
	{
		float heights         = SPREAD_MIN_HEIGHTS *
		                        pow( SPREAD_MAX_HEIGHTS / SPREAD_MIN_HEIGHTS, spread );
		// Blocks are square, so one texel is BlockPixels pixels on both axes and
		// a radius in texels is isotropic in pixels.
		float texelsPerSecond = heights * FrameRes.y / max( BlockPixels, 1.0 );
		float stepTexels      = min( texelsPerSecond * DeltaTime, MAX_STEP_TEXELS );

		// One pass is one DeltaTime of elapsed time, so the attenuation applied
		// per pass is a half-life and n frames covering T seconds attenuate by
		// exp2(-T/halfLife) whatever n happens to be.
		float retention = exp2( -DeltaTime / SPREAD_HALF_LIFE );

		float reached = Sanitise( texture( PrevDamage, uv ).r );

		// Uniform branch: at a crawl the ring lands back on the centre texel and
		// costs sixteen fetches to learn nothing.
		if( stepTexels > 0.01 )
		{
			vec2 texel = 1.0 / FlowRes;
			for( int i = 0; i < 8; ++i )
			{
				vec2 offset = SPREAD_DIRS[ i ] * stepTexels * texel;
				// Two radii per direction. With a single ring, a step of more
				// than one texel jumps over the texels in between and the front
				// arrives perforated.
				reached = max( reached, Sanitise( texture( PrevDamage, uv + offset ).r ) );
				reached = max( reached, Sanitise( texture( PrevDamage, uv + offset * 0.5 ).r ) );
			}
		}

		propagated = reached * retention;
	}

	float damage = max( seeded, propagated );
	if( damage < SPREAD_FLOOR )
		damage = 0.0;

	fragColor = vec4( Sanitise( damage ), 0.0, 0.0, 1.0 );
}
