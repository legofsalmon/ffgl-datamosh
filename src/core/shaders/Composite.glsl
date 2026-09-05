// Final pass into the host's framebuffer, and the plugin's only self-diagnostic.
//
// Kept separate from the mosh pass so the accumulation buffer stays in straight
// alpha and unclamped: it is fed back into itself, and premultiplying or
// clamping it would compound every frame.
//
// No #version line: MoshCommon.glsl is prepended at compile time and carries it,
// along with the field sampling and the gate. The Motion and Gate views call
// exactly the functions the mosh pass called, on exactly the textures and
// uniform values it was handed — MoshPipeline records them in PassMosh — so this
// cannot draw a field the warp did not use.
//
// Note what is NOT a uniform here: DeltaTime. Every quantity these views draw is
// in seconds or is a spatial term, so none of them can become frame-rate
// dependent. Its absence is the guarantee, not a comment.

uniform sampler2D Accum;
uniform sampler2D CurColor;
uniform sampler2D Flow;      // flowHistory.Delayed(motionLag) — the field Mosh used
uniform sampler2D State;     // 1x1 control state; .r is the mosh level
uniform sampler2D MaskLuma;  // the luma the mask was taken from
uniform sampler2D Damage;    // the spreading damage field, at block resolution

uniform float Mix;   // wet/dry against the untouched input; Result view only
uniform int   View;  // 0 Result, 1 Motion, 2 Gate

// Everything below is a replay of what PassMosh was handed, never a re-read of
// the live parameters.
uniform vec2  FrameRes;
uniform vec2  FlowRes;
uniform float Softness;
uniform float Corruption;
uniform float CorruptEpoch;
uniform float ThresholdPixels;
uniform float ThresholdFullScale;  // what full travel of Motion Threshold means
uniform float Decay;
uniform float MaskAmount;
uniform bool  MaskInvert;
uniform bool  HasSpread;
uniform bool  HasHistory;

in vec2 uv;
out vec4 fragColor;

const int VIEW_MOTION = 1;
const int VIEW_GATE   = 2;

// Longest hold the brightness ramp resolves. Past this everything reads as
// "held indefinitely", which it effectively is on a live stage.
const float GATE_MAX_SECONDS = 4.0;
const float GATE_MIN_SECONDS = 0.05;

vec3 HueToRgb( float h )
{
	return clamp( abs( mod( h * 6.0 + vec3( 0.0, 4.0, 2.0 ), 6.0 ) - 3.0 ) - 1.0, 0.0, 1.0 );
}

void main()
{
	vec4 colour;

	if( View == VIEW_MOTION || View == VIEW_GATE )
	{
		// The delayed field, the Softness blocky/smooth mix and the corruption
		// roll, all from the shared source. Not the current field: the warp is
		// fed one from Motion Lag frames ago, and showing the current one would
		// be a picture of a decision that was not taken.
		vec2 flow = HasHistory
		          ? MoshFlowAt( Flow, uv, FlowRes, Softness, Corruption, CorruptEpoch )
		          : vec2( 0.0 );

		// The same measure the gate uses: pixels per frame, before Motion Gain,
		// before Direction and before Quantise.
		float motionPixels = length( flow * FrameRes );

		if( View == VIEW_MOTION )
		{
			// Hue is direction: red right, yellow-green up, cyan left, violet
			// down. atan(0,0) is undefined in GLSL and a still frame is exactly
			// that case, so a still region takes hue 0 and is drawn black by its
			// zero brightness rather than by an undefined value that could reach
			// the host's framebuffer as a NaN.
			float hue = motionPixels > 1e-6
			          ? fract( atan( flow.y, flow.x ) * 0.15915494 )
			          : 0.0;

			// Brightness is magnitude on the SAME ruler as Motion Threshold:
			// full scale here is full travel there, and the square root inverts
			// that slider's t² taper. So a region's brightness IS the Motion
			// Threshold setting at which it would sit exactly on its gate — half
			// bright means threshold 0.5 shuts it. That coupling is the reason
			// for normalising against the threshold's own range rather than
			// against the pyramid's reach, which changes with Quality and would
			// make the same footage read differently at every setting.
			float scale = motionPixels / max( ThresholdFullScale, 1e-6 );
			float value = sqrt( clamp( scale, 0.0, 1.0 ) );

			// Past full scale the hue washes out to white, so "faster than the
			// threshold slider can reach" is visibly different from "exactly at
			// the top of it".
			float over = clamp( scale - 1.0, 0.0, 1.0 );
			vec3  tint = mix( HueToRgb( hue ), vec3( 1.0 ), over );

			colour = vec4( tint * value, 1.0 );
		}
		else
		{
			// "Why is nothing happening here" has four different answers now,
			// so the view has to distinguish four things:
			//
			//   black        no motion — there is nothing to gate
			//   red          motion, rejected by Motion Threshold. Brighter the
			//                closer it came to clearing it.
			//   green        the motion gate is open
			//   cyan         the gate is shut but the creep has reached here.
			//                Without this the spread is not legible at all.
			//   toward grey  the mask is closing it. Desaturating rather than
			//                darkening, because black is already spoken for by
			//                "no motion" and the two must not look alike.
			//
			// Brightness is how long a held pixel is held, in SECONDS, log
			// mapped — never the per-frame survival fraction, which is 0.94 at
			// 60fps against 0.883 at 30 for the same slider and is saturated
			// above 0.87 for almost the whole of Mosh Amount's travel. Seconds
			// are what the operator set and mean the same thing at any rate.
			float moshLevel = HasHistory ? texture( State, vec2( 0.5 ) ).r : 0.0;
			float gate      = HasHistory ? MoshGate( motionPixels, ThresholdPixels ) : 0.0;

			float damage = 0.0;
			if( HasSpread && HasHistory )
			{
				vec2 blockCentre = ( floor( uv * FlowRes ) + 0.5 ) / FlowRes;
				damage           = MoshDamageOpening( texture( Damage, blockCentre ).r );
			}

			float shaped = smoothstep( 0.0, 1.0, clamp( texture( MaskLuma, uv ).r, 0.0, 1.0 ) );
			if( MaskInvert )
				shaped = 1.0 - shaped;
			float mask = mix( 1.0, shaped, clamp( MaskAmount, 0.0, 1.0 ) );

			// Exactly the term the mosh pass built, before normalisation — the
			// un-normalised one, because seconds are what is being drawn.
			float opening = max( gate, damage );

			// The shorter of the two clocks is the one that governs, so it is
			// the one worth showing.
			float seconds = min( MoshHoldSeconds( moshLevel ), MoshDecaySeconds( Decay ) );
			float held    = clamp( log( max( seconds, GATE_MIN_SECONDS ) / GATE_MIN_SECONDS ) /
			                       log( GATE_MAX_SECONDS / GATE_MIN_SECONDS ),
			                       0.0, 1.0 );

			// How close a shut pixel came to opening.
			float nearMiss = ThresholdPixels > 0.0
			               ? clamp( motionPixels / ThresholdPixels, 0.0, 1.0 )
			               : 0.0;

			vec3 shut = mix( vec3( 0.03, 0.03, 0.05 ), vec3( 0.55, 0.06, 0.06 ), nearMiss );
			// Floored at a quarter rather than at zero: Mosh Amount lives at 0
			// between cues, and an operator checking the gate there must still
			// see its shape instead of a black frame.
			float brightness = mix( 0.25, 1.0, held );
			// Green for the motion gate, cyan where the creep is doing the work.
			vec3  open       = mix( vec3( 0.10, 1.00, 0.35 ), vec3( 0.10, 0.85, 1.00 ),
			                        clamp( damage - gate, 0.0, 1.0 ) ) * brightness;

			vec3 lit = mix( shut, open, opening );
			// The mask takes it toward neutral, never toward black.
			colour = vec4( mix( vec3( dot( lit, vec3( 0.2126, 0.7152, 0.0722 ) ) ) * 0.5,
			                    lit, mask ),
			               1.0 );
		}
	}
	else
	{
		// Result, and the landing place for any View value this build does not
		// recognise — a comp saved by a later build must come back as the
		// effect, not as a diagnostic.
		vec4 wet = texture( Accum, uv );
		vec4 dry = texture( CurColor, uv );
		colour   = mix( dry, wet, clamp( Mix, 0.0, 1.0 ) );
	}

	// One tail for every mode. Resolume expects premultiplied colour inside the
	// range its video engine works in, and a branch that skipped this could hand
	// the host an unpremultiplied or out-of-range frame. For the debug views
	// alpha is exactly 1 and the colours are already inside 0..1, so it is an
	// identity — kept anyway, because "this branch happens not to need it" is
	// how the next branch gets written without it.
	colour.a   = clamp( colour.a, 0.0, 1.0 );
	colour.rgb = clamp( colour.rgb * colour.a, vec3( 0.0 ), vec3( colour.a ) );

	fragColor = colour;
}
