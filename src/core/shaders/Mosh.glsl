// The effect itself: motion-compensate the accumulation buffer and decide,
// per pixel, whether it is allowed to refresh from the live frame.
//
// That one decision is the whole of datamoshing. A decoder that has lost its
// keyframe has no choice but to keep displacing the pixels it already has along
// whatever motion vectors arrive. Here the choice is a dial, so the same code
// path produces all three classic looks:
//
//   melt   — refresh disabled while motion keeps flowing (Amount up)
//   bloom  — vector field frozen, so motion piles up (Smoothing up)
//   drag   — only strongly moving blocks smear (Threshold up)
//
// No #version line: MoshCommon.glsl is prepended to this source at compile time
// and carries it. The field sampling, the gate and the two time curves live
// there because Composite.glsl has to be able to redraw exactly this decision,
// and a second copy of them would drift.

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
uniform float BlockRepeat;
uniform float ChromaDrift;
uniform float CorruptEpoch;
uniform float DeltaTime;
uniform bool  HasHistory;

in vec2 uv;
out vec4 fragColor;

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

	vec2 flow = MoshFlowAt( Flow, uv, FlowRes, Softness, Corruption, CorruptEpoch );

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

	// A block whose data never arrived shows whatever the decoder had to hand,
	// which is usually the block next door rather than its own history.
	vec2 sampleUV = uv + offset;
	if( BlockRepeat > 0.0 )
	{
		vec2  blockId = floor( uv * FlowRes );
		float roll    = MoshHash( blockId, CorruptEpoch + 91.0 );
		if( roll < BlockRepeat )
		{
			vec2 direction = vec2( MoshHash( blockId, CorruptEpoch + 7.0 ),
			                       MoshHash( blockId, CorruptEpoch + 13.0 ) ) * 2.0 - 1.0;
			sampleUV += sign( direction ) / FlowRes;
		}
	}

	vec4 moshed;
	if( ChromaDrift > 0.0 )
	{
		// Channels displaced by slightly different amounts, the colour fringing
		// that shows up when a damaged block's planes disagree.
		vec2 drift = offset * ChromaDrift;
		moshed.r = texture( AccumPrev, sampleUV + drift ).r;
		moshed.g = texture( AccumPrev, sampleUV ).g;
		moshed.b = texture( AccumPrev, sampleUV - drift ).b;
		moshed.a = texture( AccumPrev, sampleUV ).a;
	}
	else
	{
		moshed = texture( AccumPrev, sampleUV );
	}

	// The gate stays OUTSIDE the hold map, as a linear multiplier. Folded into
	// the exponent it would lift a barely-moving pixel to a near-full hold and
	// erase Motion Threshold.
	float gate        = MoshGate( motionPixels, ThresholdPixels );
	float retention   = MoshRetention( Decay, DeltaTime );
	float keep        = MoshHold( moshLevel, DeltaTime );
	float persistence = clamp( keep * gate * retention, 0.0, 1.0 );

	vec4 result = mix( live, moshed, persistence );

	// Last line of defence for the accumulation buffer. Anything non-finite
	// that reaches here would be re-read next frame and never leave.
	if( any( isnan( result ) ) || any( isinf( result ) ) )
		result = live;

	fragColor = result;
}
