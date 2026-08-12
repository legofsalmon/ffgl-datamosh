#version 410 core

// The effect itself: motion-compensate the accumulation buffer and decide,
// per pixel, whether it is allowed to refresh from the live frame.
//
// That one decision is the whole of datamoshing. A decoder that has lost its
// keyframe has no choice but to keep displacing the pixels it already has along
// whatever motion vectors arrive. Here the choice is a dial, so the same code
// path produces all three classic looks:
//
//   melt   — refresh disabled while motion keeps flowing (Amount up)
//   bloom  — vector field frozen, so motion piles up (Freeze up)
//   drag   — only strongly moving blocks smear (Threshold up)

uniform sampler2D CurColor;   // this frame, straight alpha, exact size
uniform sampler2D AccumPrev;  // what we displayed last frame
uniform sampler2D Flow;       // conditioned vector field, at block resolution
uniform sampler2D State;      // 1x1 control state; .r is the mosh level

uniform vec2  FrameRes;
uniform vec2  FlowRes;
uniform float MotionGain;
uniform float Softness;         // 0 hard macroblocks, 1 smooth per-pixel
uniform float PelSnap;          // 0..1 snap the warp to whole pixels
uniform float Direction;        // +1 or -1
uniform float ThresholdPixels;  // motion below this does not mosh
uniform float Decay;
uniform float Corruption;
uniform float ChromaDrift;
uniform float FrameSeed;
uniform bool  HasHistory;

in vec2 uv;
out vec4 fragColor;

float Hash( vec2 p, float seed )
{
	vec3 p3 = fract( vec3( p.x, p.y, p.x ) * 0.1031 + seed * 0.0973 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

void main()
{
	vec4 live = texture( CurColor, uv );

	// Before there is any history to displace, the input is the only honest
	// answer. This is the first frame, and the first frame after every resize.
	if( !HasHistory )
	{
		fragColor = live;
		return;
	}

	float moshLevel = texture( State, vec2( 0.5 ) ).r;

	// Sampling the field at the block centre gives every pixel in a macroblock
	// the same vector, which is what produces hard-edged MPEG tearing. Sampling
	// it normally lets the hardware interpolate, which liquefies it. Softness
	// crossfades between the two, so one estimator covers both looks.
	vec2 blockUV    = ( floor( uv * FlowRes ) + 0.5 ) / FlowRes;
	vec2 flowBlocky = texture( Flow, blockUV ).xy;
	vec2 flowSmooth = texture( Flow, uv ).xy;
	vec2 flow       = mix( flowBlocky, flowSmooth, clamp( Softness, 0.0, 1.0 ) );

	// Corrupt macroblocks: a fraction of blocks get a plausible but wrong
	// vector, mimicking a block whose data arrived damaged.
	if( Corruption > 0.0 )
	{
		vec2  blockId = floor( uv * FlowRes );
		float roll    = Hash( blockId, FrameSeed );
		if( roll < Corruption )
		{
			float angle = Hash( blockId, FrameSeed + 17.0 ) * 6.2831853;
			float scale = Hash( blockId, FrameSeed + 41.0 ) * 2.0;
			flow = mat2( cos( angle ), -sin( angle ), sin( angle ), cos( angle ) ) * flow * scale;
		}
	}

	// The gate is measured on the estimated motion, before gain, so turning
	// Gain up smears harder without also changing what counts as moving.
	float motionPixels = length( flow * FrameRes );

	vec2 offset = flow * MotionGain * Direction;

	// Whole-pixel snapping. Every frame resamples the accumulation buffer, so
	// with bilinear filtering the image blurs away within a second or two.
	// Snapping makes the fetch land on texel centres and keeps blocks crisp —
	// which is also why real codecs work in fixed pel units.
	vec2 offsetPixels = offset * FrameRes;
	offset = mix( offset, round( offsetPixels ) / FrameRes, clamp( PelSnap, 0.0, 1.0 ) );

	vec4 moshed;
	if( ChromaDrift > 0.0 )
	{
		// Channels displaced by slightly different amounts, the colour fringing
		// that shows up when a damaged block's planes disagree.
		vec2 drift = offset * ChromaDrift;
		moshed.r = texture( AccumPrev, uv + offset + drift ).r;
		moshed.g = texture( AccumPrev, uv + offset ).g;
		moshed.b = texture( AccumPrev, uv + offset - drift ).b;
		moshed.a = texture( AccumPrev, uv + offset ).a;
	}
	else
	{
		moshed = texture( AccumPrev, uv + offset );
	}

	// Still regions keep refreshing normally; moving ones carry old pixels
	// forward. Threshold at 0 moshes everything, which gives the full melt.
	float gate = smoothstep( ThresholdPixels, ThresholdPixels + 0.5, motionPixels );

	float persistence = clamp( moshLevel * gate * ( 1.0 - clamp( Decay, 0.0, 1.0 ) ), 0.0, 1.0 );

	vec4 result = mix( live, moshed, persistence );

	// Last line of defence for the accumulation buffer. Anything non-finite
	// that reaches here would be re-read next frame and never leave.
	if( any( isnan( result ) ) || any( isinf( result ) ) )
		result = live;

	fragColor = result;
}
