#version 410 core

// Conditions the raw search result into the field the warp actually uses:
// spatial coherence, temporal inertia, and hard safety limits.
//
// This pass owns the two controls that decide the character of the effect.
// Softness blurs the field, taking it from hard macroblocks toward liquid.
// Smoothing carries it forward in time, which is what makes motion leave
// trails instead of snapping frame to frame.

uniform sampler2D RawFlow;   // this frame's search result
uniform sampler2D PrevFlow;  // last frame's conditioned field

uniform vec2  FlowRes;
uniform vec2  FrameRes;
uniform float Smoothing;   // 0..1 temporal inertia, a log-scaled hold time
uniform float DeltaTime;   // seconds since the last advance
uniform float Softness;    // 0..1 spatial blur of the field
uniform float Quantise;    // 0..1 coarseness of the vector grid
uniform float BlockPixels; // macroblock edge, the coarsest sensible grid step
uniform float MaxPixels;   // hard cap on displacement per frame, in pixels
uniform bool  HasHistory;  // false on the first frame after a resize

in vec2 uv;
out vec4 fragColor;

/// Rejects non-finite values before they can enter the feedback buffers.
/// A single NaN in a field that feeds back into itself is permanent: it spreads
/// through the blur every frame and only a host restart clears it.
vec2 Sanitise( vec2 v )
{
	return ( any( isnan( v ) ) || any( isinf( v ) ) ) ? vec2( 0.0 ) : v;
}

void main()
{
	vec2 texel = 1.0 / FlowRes;

	vec4 centre = texture( RawFlow, uv );
	vec2 raw    = Sanitise( centre.xy );

	// 3x3 tent blur. Blending toward it rather than replacing keeps Softness at
	// 0 exactly as sharp as the search produced.
	vec2 blurred = vec2( 0.0 );
	float weightSum = 0.0;
	for( int y = -1; y <= 1; ++y )
	{
		for( int x = -1; x <= 1; ++x )
		{
			float weight = ( x == 0 ? 2.0 : 1.0 ) * ( y == 0 ? 2.0 : 1.0 );
			blurred += Sanitise( texture( RawFlow, uv + vec2( x, y ) * texel ).xy ) * weight;
			weightSum += weight;
		}
	}
	blurred /= weightSum;

	vec2 conditioned = mix( raw, blurred, Softness );

	vec2 previous = HasHistory ? Sanitise( texture( PrevFlow, uv ).xy ) : vec2( 0.0 );

	// Temporal inertia, as a hold time on a log scale rather than a raw
	// per-frame blend — the same treatment as Mosh Amount, for the same two
	// reasons: a raw blend crushes its range into the top of the travel, and
	// it means something different at every frame rate. At 1 the field stops
	// updating and the warp keeps applying the same vectors every frame, which
	// is P-frame duplication and produces the bloom.
	//
	// There used to be a second slider, Freeze, applied right here as another
	// mix toward the same target. Two successive mixes toward one target
	// compose to a single mix, 1-(1-S)(1-F), symmetric in the two — measured
	// identical to every digit — so it was one axis with two names, and it was
	// folded into this one.
	const float SMOOTH_MIN_SECONDS = 0.016;
	const float SMOOTH_MAX_SECONDS = 2.0;
	float s = clamp( Smoothing, 0.0, 1.0 );
	float inertia;
	if( s >= 0.995 )
	{
		inertia = 1.0;
	}
	else
	{
		float hold = SMOOTH_MIN_SECONDS * pow( SMOOTH_MAX_SECONDS / SMOOTH_MIN_SECONDS, s );
		inertia    = exp2( -DeltaTime / hold ) * clamp( s / 0.05, 0.0, 1.0 );
	}
	conditioned = mix( conditioned, previous, inertia );

	// Coarsen the vector grid. A real encoder cannot afford to describe motion
	// precisely at low bitrates, and the stepped movement that produces is a
	// large part of what reads as "compressed" rather than "warped".
	if( Quantise > 0.0 )
	{
		float stepPixels = mix( 0.5, max( 1.0, BlockPixels * 0.5 ), clamp( Quantise, 0.0, 1.0 ) );
		vec2  stepUV     = vec2( stepPixels ) / FrameRes;
		conditioned      = round( conditioned / stepUV ) * stepUV;
	}

	// Hard cap. Without it a bad match near a cut can produce a vector that
	// samples half a screen away and smears the whole frame in one step.
	vec2  maxUV     = vec2( MaxPixels ) / FrameRes;
	float magnitude = length( conditioned / max( maxUV, vec2( 1e-6 ) ) );
	if( magnitude > 1.0 )
		conditioned /= magnitude;

	conditioned = Sanitise( conditioned );

	// Match residual, carried through for the cut detector and the warp's gate.
	float residual = clamp( centre.z, 0.0, 1.0 );

	fragColor = vec4( conditioned, residual, 1.0 );
}
