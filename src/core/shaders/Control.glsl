#version 410 core

// A 1x1 state machine deciding how strongly the frame is moshing right now.
//
// This runs on the GPU, into a one-texel buffer, rather than on the CPU where
// it would be easier to write. The reason is that the cut detector needs the
// reduced scene-difference value, and reading that back would mean a
// glReadPixels — a full GPU/CPU sync in the middle of a live render. One
// stalled frame is a visible hitch on stage. Keeping the decision on the GPU
// costs one trivial draw and never blocks.
//
// State layout:
//   .r  mosh level currently applied, 0..1
//   .g  seconds remaining in a triggered burst
//   .b  position in bars, whole bars plus phase, for beat-edge detection
//   .a  running baseline of scene difference, for adaptive cut detection

uniform sampler2D State;      // previous frame's state
uniform sampler2D SceneDiff;  // difference map, reduced via its mip chain
uniform float     DiffLevel;  // the 1x1 level of SceneDiff

uniform float MoshAmount;
uniform bool  Trigger;
uniform bool  Reset;
uniform int   AutoMode;     // 0 manual, 1 on cut, 2 on beat
uniform float Sensitivity;
uniform float Duration;
uniform float AudioLevel;
uniform float AudioAmount;
uniform float BarPhase;
uniform float BeatDivisor;
uniform float DeltaTime;
uniform bool  HasHistory;

out vec4 fragColor;

// Seconds for the mosh level to fall away once a burst ends. An instant drop
// reads as a second cut, which is not what a burst should look like.
const float RELEASE_SECONDS = 0.25;

// How fast the running baseline of frame-to-frame change adapts. About a fifth
// of a second at 60fps: long enough to average over a shot, short enough that
// it does not carry its starting guess into the first seconds of a set.
const float BASELINE_RATE = 0.08;

// Resolume reports position within a bar and assumes four beats to it.
const float BEATS_PER_BAR = 4.0;

// Bar position wraps here to stay in a range a float can resolve finely. 256 is
// a multiple of four bars, which is the longest divisor this offers (16 beats),
// so the wrap lands exactly on a beat boundary and cannot fire spuriously.
const float BAR_WRAP = 256.0;

void main()
{
	vec4 previous = HasHistory ? texture( State, vec2( 0.5 ) ) : vec4( 0.0, 0.0, 0.0, 0.0 );

	float moshLevel   = previous.r;
	float burst       = previous.g;
	float lastBarPos  = previous.b;
	float baseline    = previous.a;

	// Mean absolute frame difference: the top of the difference map's mip chain.
	float difference = textureLod( SceneDiff, vec2( 0.5 ), DiffLevel ).r;

	// Sensitivity 0..1 maps to a demanding threshold at 0 and a twitchy one at 1.
	float absoluteThreshold = mix( 0.30, 0.02, clamp( Sensitivity, 0.0, 1.0 ) );
	float relativeThreshold = mix( 6.0, 1.6, clamp( Sensitivity, 0.0, 1.0 ) );

	// Compare against a running average rather than a fixed number, so the
	// detector works on both a dark ambient clip and a strobing one.
	//
	// The first frame has no previous frame, so its "difference" is the whole
	// image against black — a reading that says nothing about the footage.
	// Seeding from it would poison the baseline for as long as the average
	// remembers. Starting at the absolute threshold instead is a neutral guess:
	// high enough that nothing can trigger before real footage has been seen,
	// and it converges on the truth within a fraction of a second.
	float nextBaseline = HasHistory ? mix( baseline, difference, BASELINE_RATE )
	                                : absoluteThreshold;

	bool cutDetected = HasHistory &&
	                   difference > absoluteThreshold &&
	                   difference > baseline * relativeThreshold;

	// Beat edge.
	//
	// The host only reports position *within* the current bar, which on its own
	// cannot express a divisor longer than a bar: at 8 or 16 beats the phase
	// never crosses a division and every setting above 4 collapses into firing
	// once per bar. Counting bars as they wrap gives a position that keeps
	// climbing, so any divisor works.
	float lastFraction = fract( lastBarPos );
	float barPosition  = floor( lastBarPos ) + BarPhase;
	if( BarPhase < lastFraction )
		barPosition += 1.0;
	barPosition = mod( barPosition, BAR_WRAP );

	float divisor   = max( 1.0, BeatDivisor );
	float ticks     = barPosition * BEATS_PER_BAR / divisor;
	float lastTicks = lastBarPos * BEATS_PER_BAR / divisor;
	bool  beatEdge  = HasHistory && floor( ticks ) != floor( lastTicks );

	bool fire = Trigger ||
	            ( AutoMode == 1 && cutDetected ) ||
	            ( AutoMode == 2 && beatEdge );

	if( fire )
		burst = max( Duration, 0.0 );

	// Held before the countdown, not after, so a burst shorter than one frame
	// still produces one frame of mosh. Otherwise a short Burst Length — or any
	// setting at all on a host running at 30fps — expires inside the frame that
	// started it and the trigger appears to do nothing.
	float held = ( fire || burst > 0.0 ) ? 1.0 : 0.0;

	burst = max( 0.0, burst - DeltaTime );
	float target = clamp( max( MoshAmount + AudioLevel * AudioAmount, held ), 0.0, 1.0 );

	// Rise immediately, fall gradually.
	moshLevel = ( target >= moshLevel )
	              ? target
	              : max( target, moshLevel - DeltaTime / RELEASE_SECONDS );

	if( Reset || !HasHistory )
	{
		moshLevel = 0.0;
		burst     = 0.0;
	}

	if( isnan( moshLevel ) || isinf( moshLevel ) )
		moshLevel = 0.0;

	fragColor = vec4( clamp( moshLevel, 0.0, 1.0 ), burst, barPosition, nextBaseline );
}
