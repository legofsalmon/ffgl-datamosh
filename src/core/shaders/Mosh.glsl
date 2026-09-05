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
uniform sampler2D MaskLuma;   // this frame's luma, from the MOTION input

uniform vec2  FrameRes;
uniform vec2  FlowRes;
uniform float MotionGain;
uniform float Softness;         // 0 hard macroblocks, 1 smooth per-pixel
uniform float PelSnap;          // 0..1 snap the warp to whole pixels
uniform float Direction;        // +1 or -1
uniform float ThresholdPixels;  // motion below this does not mosh
uniform float Quantise;         // 0..1 coarseness of the displacement grid
uniform float BlockPixels;      // macroblock edge, the coarsest sensible step
uniform float MaskAmount;       // 0 the mask is inert, 1 fully applied
uniform bool  MaskInvert;       // mosh the dark parts instead
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

	// The gate is measured on the estimated motion, before gain and before the
	// quantiser, so turning Gain up smears harder and coarsening the grid
	// stepifies the movement — neither changes what counts as moving.
	float motionPixels = length( flow * FrameRes );

	// Coarsen the grid only now, so a vector that rounds away cannot take the
	// gate with it. See MoshQuantise.
	flow = MoshQuantise( flow, Quantise, BlockPixels, FrameRes );

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
	float retention = MoshRetention( Decay, DeltaTime );
	float keep      = MoshHold( moshLevel, DeltaTime );

	// Where the mosh is allowed to land, from the brightness of the motion
	// input — the clip's own in the effect, the other layer's in the mixer, so
	// the layer supplying the motion also paints the mask, and Motion Source
	// flips both together.
	//
	// Sampled at uv, not at sampleUV: the mask says where the mosh lands, not
	// what it fetches. Sampling it displaced would drag the mask along with the
	// warp and smear its own edges into the image.
	//
	// smoothstep rather than raw luma because real footage sits mid-grey almost
	// everywhere, and a raw mask on it reads as a flat attenuation rather than
	// as a shape.
	float shaped = smoothstep( 0.0, 1.0, clamp( texture( MaskLuma, uv ).r, 0.0, 1.0 ) );
	if( MaskInvert )
		shaped = 1.0 - shaped;
	// Amount is depth, not gain: at 0 this is exactly 1 everywhere and every
	// existing composition renders unchanged.
	float mask = mix( 1.0, shaped, clamp( MaskAmount, 0.0, 1.0 ) );

	// One spatial term, normalised once. The gate asks whether this pixel is
	// moving and the mask asks whether it is allowed to hold; both vary across
	// the frame, so both belong in here rather than each inventing its own
	// frame-rate treatment beside the other. Three normalisations with three
	// comments is how divergence starts.
	//
	// The mask stays OUTSIDE the hold-time map for the same reason the gate
	// does. Folded into the exponent, a half-lit pixel would sit at a 0.45s
	// hold against a fully lit pixel's 4s — perceptually both "held" — so the
	// mask would collapse into a hard key and lose every midtone. Out here,
	// mask 0.5 is literally half the cross-fade weight, and that gradient is
	// what makes it paintable.
	float spatial     = MoshGate( motionPixels, ThresholdPixels ) * mask;
	float persistence = clamp( keep * retention * MoshNormaliseSpatial( spatial, DeltaTime ),
	                           0.0, 1.0 );

	vec4 result = mix( live, moshed, persistence );

	// Last line of defence for the accumulation buffer. Anything non-finite
	// that reaches here would be re-read next frame and never leave.
	if( any( isnan( result ) ) || any( isinf( result ) ) )
		result = live;

	fragColor = result;
}
