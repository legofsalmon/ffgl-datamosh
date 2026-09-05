#include "harness/Synthetic.h"
#include "harness/TestRunner.h"

#include <MoshParams.h>
#include <MoshPipeline.h>
#include <RenderTarget.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace datamosh::test {

namespace {

/// Drives the pipeline the way a host would: upload a frame, advance, repeat.
struct Rig
{
	MoshPipeline pipeline;
	InputTexture texture;
	int          width  = 0;
	int          height = 0;

	bool Setup( int w, int h )
	{
		width  = w;
		height = h;
		return pipeline.Initialise() && texture.Create( w, h );
	}

	FrameInputs Inputs() const
	{
		FrameInputs inputs;
		inputs.pixelTexture  = texture.GetHandle();
		inputs.motionTexture = texture.GetHandle();
		inputs.width         = width;
		inputs.height        = height;
		return inputs;
	}

	/// Pushes a frame of the reference pattern displaced by (shiftX, shiftY).
	void PushShifted( float shiftX, float shiftY, MoshParams params, int frameIndex )
	{
		texture.Upload( MakeShiftedPattern( width, height, shiftX, shiftY ) );
		params.frame = frameIndex;
		pipeline.Advance( Inputs(), params );
	}

	void PushImage( const std::vector< uint8_t >& rgba, MoshParams params, int frameIndex )
	{
		texture.Upload( rgba );
		params.frame = frameIndex;
		pipeline.Advance( Inputs(), params );
	}

	void Teardown()
	{
		texture.Release();
		pipeline.Release();
	}
};

/// Parameters that isolate the estimator: no temporal blending, no blurring,
/// nothing that would mask a wrong vector behind smoothing.
MoshParams RawEstimatorParams()
{
	MoshParams params;
	params.motionSmoothing = 0.0f;
	params.softness        = 0.0f;
	params.blockSize       = 16;
	params.quality         = Quality::High;
	params.deltaTime       = 1.0f / 60.0f;
	return params;
}

/// Mean of one component, ignoring a margin of blocks around the edge.
///
/// Border blocks are genuinely unknowable: their content came from outside the
/// frame, so no search can recover it. Including them would be measuring the
/// wrong thing.
float MeanInterior( const std::vector< float >& rgba, int width, int height, int component, int margin )
{
	double total = 0.0;
	int    count = 0;
	for( int y = margin; y < height - margin; ++y )
	{
		for( int x = margin; x < width - margin; ++x )
		{
			total += rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + component ];
			++count;
		}
	}
	return count == 0 ? 0.0f : static_cast< float >( total / count );
}

/// Mean absolute difference on one half of the frame, excluding a margin.
///
/// A whole-frame mean cannot see a mask: it averages the two halves back
/// together and reports the same number whichever side moshed.
float HalfDifference( const std::vector< float >& a, const std::vector< float >& b,
                      int width, int height, int margin, bool leftHalf )
{
	const int x0 = leftHalf ? margin : width / 2 + margin;
	const int x1 = leftHalf ? width / 2 - margin : width - margin;
	double    total = 0.0;
	int       count = 0;
	for( int y = margin; y < height - margin; ++y )
	{
		for( int x = x0; x < x1; ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			total += std::fabs( a[ i ] - b[ i ] );
			++count;
		}
	}
	return count == 0 ? -1.0f : static_cast< float >( total / count );
}

/// Mean absolute difference across the vector components only.
///
/// The flow buffer also carries the match residual, which is a diagnostic
/// recomputed every frame. Comparing it would measure the wrong thing.
float MeanVectorDifference( const std::vector< float >& a, const std::vector< float >& b )
{
	if( a.size() != b.size() || a.empty() )
		return -1.0f;
	double total = 0.0;
	size_t count = 0;
	for( size_t index = 0; index + 1 < a.size(); index += 4 )
	{
		total += std::fabs( a[ index ] - b[ index ] );
		total += std::fabs( a[ index + 1 ] - b[ index + 1 ] );
		count += 2;
	}
	return count == 0 ? -1.0f : static_cast< float >( total / count );
}

constexpr int FRAME_WIDTH  = 256;
constexpr int FRAME_HEIGHT = 192;

}  // namespace

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

TEST( ShadersCompileAndPipelineInitialises )
{
	MoshPipeline pipeline;
	// The single most valuable assertion here: Resolume swallows GLSL
	// compilation errors silently, so a broken shader ships as a black frame
	// with no diagnostic anywhere.
	CHECK( pipeline.Initialise() );
	CHECK( pipeline.IsInitialised() );
	pipeline.Release();
	CHECK( !pipeline.IsInitialised() );
}

// ---------------------------------------------------------------------------
// Resource lifetime
// ---------------------------------------------------------------------------

TEST( RenderTargetReleaseFreesBothTextureAndFramebuffer )
{
	// A direct regression test for the bug in ffglex::FFGLFBO::Release(), where
	// the colour texture is never deleted because the second guard re-tests the
	// depth buffer. That leak is why this project has its own RenderTarget.
	RenderTarget target;
	CHECK( target.Allocate( 64, 64, GL_RGBA16F ) );

	const GLuint textureID = target.GetTexture();
	const GLuint fboID     = target.GetFBO();
	CHECK( glIsTexture( textureID ) == GL_TRUE );
	CHECK( glIsFramebuffer( fboID ) == GL_TRUE );

	target.Release();

	CHECK( glIsTexture( textureID ) == GL_FALSE );
	CHECK( glIsFramebuffer( fboID ) == GL_FALSE );
	CHECK( !target.IsValid() );
}

TEST( ResizeStormDoesNotAccumulateObjects )
{
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	// A name generated after everything has been released should be reusing a
	// freed one. If the pipeline leaked on every resize, names would climb.
	auto probeName = []() {
		GLuint id = 0;
		glGenTextures( 1, &id );
		glDeleteTextures( 1, &id );
		return id;
	};

	const GLuint before = probeName();

	MoshParams params = RawEstimatorParams();
	const int sizes[][ 2 ] = { { 128, 128 }, { 256, 192 }, { 96, 160 }, { 320, 240 }, { 64, 64 } };
	for( int pass = 0; pass < 6; ++pass )
	{
		for( const auto& size : sizes )
		{
			rig.texture.Release();
			CHECK( rig.texture.Create( size[ 0 ], size[ 1 ] ) );
			rig.width  = size[ 0 ];
			rig.height = size[ 1 ];
			rig.PushShifted( 0.0f, 0.0f, params, pass );
		}
	}

	const GLuint after = probeName();
	// Generous slack: drivers are free to hand out names in any order. What this
	// catches is unbounded growth, which is what a per-resize leak looks like.
	CHECK( after <= before + 64 );

	rig.Teardown();
}

// ---------------------------------------------------------------------------
// Motion estimation, against known answers
// ---------------------------------------------------------------------------

TEST( StaticInputProducesNoMotion )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();
	for( int frame = 0; frame < 6; ++frame )
		rig.PushShifted( 0.0f, 0.0f, params, frame );

	const std::vector< float > flow = ReadTarget( rig.pipeline.GetFlowField() );
	const int fw = rig.pipeline.GetFlowWidth();
	const int fh = rig.pipeline.GetFlowHeight();

	// A resting image must rest. Any drift here would creep across the whole
	// frame the moment the mosh gate opened.
	CHECK_NEAR( MeanInterior( flow, fw, fh, 0, 1 ), 0.0, 1e-4 );
	CHECK_NEAR( MeanInterior( flow, fw, fh, 1, 1 ), 0.0, 1e-4 );

	rig.Teardown();
}

TEST( MotionEstimationTracksKnownVelocity )
{
	struct Case
	{
		const char* name;
		float       dx;
		float       dy;
	};
	const Case cases[] = {
		{ "right", 3.0f, 0.0f },
		{ "left", -4.0f, 0.0f },
		{ "down", 0.0f, 2.0f },
		{ "diagonal", -3.0f, 3.0f },
	};

	for( const Case& testCase : cases )
	{
		Rig rig;
		CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

		MoshParams params = RawEstimatorParams();
		float shiftX = 0.0f;
		float shiftY = 0.0f;
		for( int frame = 0; frame < 8; ++frame )
		{
			rig.PushShifted( shiftX, shiftY, params, frame );
			shiftX += testCase.dx;
			shiftY += testCase.dy;
		}

		const std::vector< float > flow = ReadTarget( rig.pipeline.GetFlowField() );
		const int fw = rig.pipeline.GetFlowWidth();
		const int fh = rig.pipeline.GetFlowHeight();

		// The stored vector is backward: it points at where this block's content
		// was in the previous frame, which is the opposite of the content's
		// travel. Warping with it is motion compensation, exactly as a P-frame.
		const float expectedU = -testCase.dx / FRAME_WIDTH;
		const float expectedV = -testCase.dy / FRAME_HEIGHT;

		// Two blocks' worth of margin: the leading edge of the frame is fed by
		// content that was off-screen a frame ago and cannot be matched.
		const float measuredU = MeanInterior( flow, fw, fh, 0, 2 );
		const float measuredV = MeanInterior( flow, fw, fh, 1, 2 );

		// Tolerance of roughly one pixel.
		CHECK_NEAR( measuredU, expectedU, 1.5 / FRAME_WIDTH );
		CHECK_NEAR( measuredV, expectedV, 1.5 / FRAME_HEIGHT );

		rig.Teardown();
	}
}

TEST( FullSmoothingHoldsTheVectorField )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();

	float shift = 0.0f;
	for( int frame = 0; frame < 8; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += 3.0f;
	}
	const std::vector< float > before = ReadTarget( rig.pipeline.GetFlowField() );

	// Freeze, then move the content the other way. A frozen field must ignore
	// this completely — that is what makes the bloom keep flowing in one
	// direction while the underlying clip does something else.
	params.motionSmoothing = 1.0f;
	for( int frame = 8; frame < 14; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift -= 5.0f;
	}
	const std::vector< float > after = ReadTarget( rig.pipeline.GetFlowField() );

	CHECK( MeanVectorDifference( before, after ) >= 0.0f );
	CHECK_NEAR( MeanVectorDifference( before, after ), 0.0, 1e-6 );

	rig.Teardown();
}

// ---------------------------------------------------------------------------
// Control state
// ---------------------------------------------------------------------------

TEST( CutDetectionRaisesMoshLevel )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();
	params.autoMode    = AutoMode::OnCut;
	params.sensitivity = 0.7f;
	params.duration    = 1.0f;
	params.moshAmount  = 0.0f;

	// Settle the running baseline on ordinary moving footage.
	float shift = 0.0f;
	for( int frame = 0; frame < 20; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += 2.0f;
	}

	const std::vector< float > quiet = ReadTarget( rig.pipeline.GetControlState() );
	CHECK_NEAR( quiet[ 0 ], 0.0, 1e-3 );

	// A hard cut to something completely different.
	rig.PushImage( MakeSolid( FRAME_WIDTH, FRAME_HEIGHT, 255, 255, 255 ), params, 20 );

	const std::vector< float > fired = ReadTarget( rig.pipeline.GetControlState() );
	CHECK( fired[ 0 ] > 0.5f );

	rig.Teardown();
}

TEST( ManualModeIgnoresCuts )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();
	params.autoMode   = AutoMode::Manual;
	params.moshAmount = 0.0f;

	float shift = 0.0f;
	for( int frame = 0; frame < 12; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += 2.0f;
	}
	rig.PushImage( MakeSolid( FRAME_WIDTH, FRAME_HEIGHT, 255, 255, 255 ), params, 12 );

	const std::vector< float > state = ReadTarget( rig.pipeline.GetControlState() );
	CHECK_NEAR( state[ 0 ], 0.0, 1e-3 );

	rig.Teardown();
}

TEST( TriggerRaisesAndReleases )
{
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	MoshParams params = RawEstimatorParams();
	params.duration   = 0.1f;
	params.deltaTime  = 1.0f / 60.0f;

	rig.PushShifted( 0.0f, 0.0f, params, 0 );
	rig.PushShifted( 2.0f, 0.0f, params, 1 );

	params.trigger = true;
	rig.PushShifted( 4.0f, 0.0f, params, 2 );
	params.trigger = false;

	CHECK( ReadTarget( rig.pipeline.GetControlState() )[ 0 ] > 0.9f );

	// 0.1s of burst plus a 0.25s release, comfortably covered by 40 frames.
	for( int frame = 3; frame < 43; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );

	CHECK_NEAR( ReadTarget( rig.pipeline.GetControlState() )[ 0 ], 0.0, 1e-3 );

	rig.Teardown();
}

TEST( HoldStaysUpForAsLongAsItIsHeld )
{
	// The whole point of hold as against trigger: it outlives Burst Length. With
	// a burst of 0.1s, sixty frames at 1/60 is six times the burst, so anything
	// still up at the end is up because hold held it.
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	MoshParams params = RawEstimatorParams();
	params.duration   = 0.1f;
	params.deltaTime  = 1.0f / 60.0f;

	rig.PushShifted( 0.0f, 0.0f, params, 0 );
	rig.PushShifted( 2.0f, 0.0f, params, 1 );

	params.hold = true;
	for( int frame = 2; frame < 62; ++frame )
	{
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );
		// Not just at the end — at every frame, so a hold that decays slowly
		// enough to still read as up on the last frame cannot pass.
		CHECK( ReadTarget( rig.pipeline.GetControlState() )[ 0 ] > 0.9f );
	}

	rig.Teardown();
}

TEST( HoldReleasesWhenItIsLetGo )
{
	// The failure that matters. Hold is a level, not an edge, so a bug that
	// latches it leaves the effect stuck on — which on stage is unrecoverable
	// without pulling the effect off the layer.
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	MoshParams params = RawEstimatorParams();
	params.duration   = 0.1f;
	params.deltaTime  = 1.0f / 60.0f;

	rig.PushShifted( 0.0f, 0.0f, params, 0 );
	rig.PushShifted( 2.0f, 0.0f, params, 1 );

	params.hold = true;
	for( int frame = 2; frame < 20; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );
	CHECK( ReadTarget( rig.pipeline.GetControlState() )[ 0 ] > 0.9f );

	// Let go. Only the 0.25s release ramp should remain — no burst was ever
	// started, so Burst Length must not extend this.
	params.hold = false;
	for( int frame = 20; frame < 60; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );

	CHECK_NEAR( ReadTarget( rig.pipeline.GetControlState() )[ 0 ], 0.0, 1e-3 );

	rig.Teardown();
}

TEST( ResetClearsAHeldMosh )
{
	// The escape hatch. If a release is ever dropped — a lost MIDI note-off, a
	// window focus change mid-hold — Reset has to get the picture back even
	// while hold is still reading true. Without this, recovery means removing
	// the effect.
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	MoshParams params = RawEstimatorParams();
	params.deltaTime  = 1.0f / 60.0f;

	rig.PushShifted( 0.0f, 0.0f, params, 0 );
	rig.PushShifted( 2.0f, 0.0f, params, 1 );

	params.hold = true;
	for( int frame = 2; frame < 20; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );
	CHECK( ReadTarget( rig.pipeline.GetControlState() )[ 0 ] > 0.9f );

	// Reset while hold is still down.
	params.reset = true;
	rig.PushShifted( 40.0f, 0.0f, params, 20 );

	CHECK_NEAR( ReadTarget( rig.pipeline.GetControlState() )[ 0 ], 0.0, 1e-3 );

	rig.Teardown();
}

TEST( ReleasingHoldEndsTheMoshEvenOverARunningBurst )
{
	// Release has to mean release. A hold takes ownership of the level while it
	// is down, so no automatic burst is left running underneath it — otherwise
	// letting go changes nothing visible and reads as a control that has stuck
	// on. This is the On Beat case: at 128bpm the bursts refire every 469ms
	// while Burst Length defaults to a second, so the level would otherwise be
	// pinned continuously and the release invisible.
	Rig rig;
	CHECK( rig.Setup( 128, 128 ) );

	MoshParams params = RawEstimatorParams();
	params.duration   = 1.0f;          // a burst far longer than the hold
	params.deltaTime  = 1.0f / 60.0f;

	rig.PushShifted( 0.0f, 0.0f, params, 0 );
	rig.PushShifted( 2.0f, 0.0f, params, 1 );

	// Fire a one-second burst, then hold and release well inside it.
	params.trigger = true;
	rig.PushShifted( 4.0f, 0.0f, params, 2 );
	params.trigger = false;

	params.hold = true;
	for( int frame = 3; frame < 10; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );
	CHECK( ReadTarget( rig.pipeline.GetControlState() )[ 0 ] > 0.9f );

	params.hold = false;
	for( int frame = 10; frame < 40; ++frame )
		rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );

	// Only ~0.13s of the burst had elapsed, so without hold taking ownership
	// this would still be pinned at 1.0.
	CHECK_NEAR( ReadTarget( rig.pipeline.GetControlState() )[ 0 ], 0.0, 1e-3 );

	rig.Teardown();
}

TEST( BeatDivisorFiresAtTheRequestedInterval )
{
	// The host only reports position within the current bar. Deriving beat edges
	// from that alone silently caps the interval at one bar, so "8" and "16"
	// behave identically to "4" — a setting that appears to work and does not.
	const auto countFirings = []( int divisor, int bars ) {
		Rig rig;
		if( !rig.Setup( 64, 64 ) )
			return -1;

		constexpr int FRAMES_PER_BAR = 16;

		MoshParams params  = RawEstimatorParams();
		params.autoMode    = AutoMode::OnBeat;
		params.beatDivisor = divisor;
		params.moshAmount  = 0.0f;
		// Short enough that the level falls back between firings, so each one
		// shows up as its own rising edge.
		params.duration    = 0.05f;
		params.deltaTime   = 1.0f / FRAMES_PER_BAR;

		int   firings  = 0;
		float previous = 0.0f;

		for( int frame = 0; frame < bars * FRAMES_PER_BAR; ++frame )
		{
			params.barPhase = static_cast< float >( frame % FRAMES_PER_BAR ) / FRAMES_PER_BAR;
			rig.PushShifted( 0.0f, 0.0f, params, frame );

			const float level = ReadTarget( rig.pipeline.GetControlState() )[ 0 ];
			if( previous < 0.5f && level >= 0.5f )
				++firings;
			previous = level;
		}

		rig.Teardown();
		return firings;
	};

	constexpr int BARS = 8;

	// Within one of the exact count: the very first frame has no history to
	// compare against, so whichever beat lands on it is suppressed. The bug this
	// guards against is off by a factor of four, not by one.
	CHECK_NEAR( countFirings( 1, BARS ), BARS * 4, 1 );   // every beat
	CHECK_NEAR( countFirings( 4, BARS ), BARS, 1 );       // every bar
	CHECK_NEAR( countFirings( 8, BARS ), BARS / 2, 1 );   // every two bars
	CHECK_NEAR( countFirings( 16, BARS ), BARS / 4, 1 );  // every four bars
}

// ---------------------------------------------------------------------------
// The effect itself
// ---------------------------------------------------------------------------

TEST( BypassLeavesTheImageAlone )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();
	params.moshAmount = 0.0f;

	float shift = 0.0f;
	for( int frame = 0; frame < 6; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += 3.0f;
	}

	// With nothing held over, the accumulation buffer must be this frame's
	// image. Anything else means the effect is not neutral at zero.
	const std::vector< float > accumulated = ReadTarget( rig.pipeline.GetAccumulation() );
	const float meanRed = MeanComponent( accumulated, 0 );

	double expected = 0.0;
	for( int y = 0; y < FRAME_HEIGHT; ++y )
		for( int x = 0; x < FRAME_WIDTH; ++x )
			expected += PatternAt( x - shift + 3.0f, y );
	expected /= static_cast< double >( FRAME_WIDTH ) * FRAME_HEIGHT;

	CHECK_NEAR( meanRed, expected, 0.02 );

	rig.Teardown();
}

TEST( MotionCompensationReconstructsPureTranslation )
{
	// The sharpest end-to-end check in the suite. If the vector sign, the warp
	// direction, the pel snapping and the block-centre lookup are all correct,
	// then displacing last frame's image by the estimated motion must reproduce
	// this frame's image. Getting any one of them backwards doubles the error
	// instead of cancelling it, so this fails loudly rather than subtly.
	const auto run = []( float moshAmount ) {
		Rig rig;
		if( !rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) )
			return std::vector< float >{};

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = moshAmount;
		params.motionThreshold = 0.0f;
		params.motionGain      = 1.0f;
		params.pelSnap         = 1.0f;
		params.decay           = 0.0f;

		float shift = 0.0f;
		for( int frame = 0; frame < 10; ++frame )
		{
			rig.PushShifted( shift, 0.0f, params, frame );
			shift += 3.0f;
		}

		std::vector< float > result = ReadTarget( rig.pipeline.GetAccumulation() );
		rig.Teardown();
		return result;
	};

	const std::vector< float > live   = run( 0.0f );
	const std::vector< float > moshed = run( 1.0f );

	CHECK( !live.empty() );
	CHECK( !moshed.empty() );

	const float difference = MeanInteriorDifference( live, moshed, FRAME_WIDTH, FRAME_HEIGHT, 24 );
	CHECK( difference >= 0.0f );
	CHECK( difference < 0.05f );
}

TEST( MoshingHoldsPixelsThroughACut )
{
	// The effect proper. With the vector field frozen and refreshing disabled,
	// incoming pixels must not be able to reach the screen: the old image keeps
	// being displaced along the held motion. That is the melt.
	const auto run = []( float moshAmount, float freeze ) {
		Rig rig;
		if( !rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) )
			return -1.0f;

		MoshParams params      = RawEstimatorParams();
		params.motionThreshold = 0.0f;
		params.decay           = 0.0f;

		// Establish real motion first, so there is a field worth freezing.
		float shift = 0.0f;
		for( int frame = 0; frame < 10; ++frame )
		{
			rig.PushShifted( shift, 0.0f, params, frame );
			shift += 3.0f;
		}

		params.moshAmount   = moshAmount;
		params.motionSmoothing = freeze;

		const std::vector< uint8_t > white = MakeSolid( FRAME_WIDTH, FRAME_HEIGHT, 255, 255, 255 );
		for( int frame = 10; frame < 24; ++frame )
			rig.PushImage( white, params, frame );

		const float mean = MeanComponent( ReadTarget( rig.pipeline.GetAccumulation() ), 0 );
		rig.Teardown();
		return mean;
	};

	// Refreshing normally, the new shot arrives immediately.
	CHECK_NEAR( run( 0.0f, 0.0f ), 1.0, 0.02 );

	// Moshing, it never arrives at all.
	const float held = run( 1.0f, 1.0f );
	CHECK( held >= 0.0f );
	CHECK( held < 0.85f );
}

TEST( FlowHistoryReturnsTheFieldFromNFramesAgo )
{
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params = RawEstimatorParams();

	// Motion that changes every frame, so a field from the wrong frame is
	// obviously the wrong field rather than coincidentally similar.
	std::vector< std::vector< float > > recorded;
	const float velocities[] = { 1.0f, 4.0f, -3.0f, 2.0f, -5.0f, 3.0f, -1.0f, 6.0f, -4.0f, 2.0f };

	float shift = 0.0f;
	for( int frame = 0; frame < 10; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += velocities[ frame ];
		recorded.push_back( ReadTarget( rig.pipeline.GetFlowField() ) );
	}

	// Delayed(0) is the field just written; Delayed(n) is n frames before it.
	const int lastFrame = static_cast< int >( recorded.size() ) - 1;
	for( int framesAgo = 0; framesAgo <= 5; ++framesAgo )
	{
		const std::vector< float > delayed = ReadTarget( rig.pipeline.GetDelayedFlowField( framesAgo ) );
		CHECK_NEAR( MeanVectorDifference( delayed, recorded[ lastFrame - framesAgo ] ), 0.0, 1e-6 );
	}

	rig.Teardown();
}

TEST( MotionLagChangesWhatTheWarpApplies )
{
	// Lag exists because an accurate estimator reconstructs the frame, which is
	// correct and far too clean. Applying a field from several frames back puts
	// motion on content it does not belong to, and that has to show.
	const auto run = []( int lag ) {
		Rig rig;
		if( !rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) )
			return std::vector< float >{};

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = 1.0f;
		params.motionThreshold = 0.0f;
		params.motionLag       = lag;

		// Direction reverses partway, so a stale field is pointing the wrong way
		// entirely rather than merely being out of date.
		float shift = 0.0f;
		for( int frame = 0; frame < 14; ++frame )
		{
			rig.PushShifted( shift, 0.0f, params, frame );
			shift += ( frame < 7 ) ? 4.0f : -4.0f;
		}

		std::vector< float > result = ReadTarget( rig.pipeline.GetAccumulation() );
		rig.Teardown();
		return result;
	};

	const std::vector< float > live    = run( 0 );
	const std::vector< float > delayed = run( 6 );

	CHECK( !live.empty() );
	CHECK( !delayed.empty() );
	CHECK( MeanInteriorDifference( live, delayed, FRAME_WIDTH, FRAME_HEIGHT, 16 ) > 0.01f );
}

TEST( DecayIsIndependentOfFrameRate )
{
	// Decay is a half-life, not a per-frame fraction. The same wall-clock second
	// must bleed the same amount of live image back in whether it arrives as 30
	// steps or 60 — otherwise the control means something different on every
	// machine, and on the same machine whenever the frame rate dips.
	const auto meanAfterOneSecond = []( int stepsPerSecond ) {
		Rig rig;
		if( !rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) )
			return -1.0f;

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = 1.0f;
		params.motionThreshold = 0.0f;
		params.decay           = 0.9f;
		// Gain of zero makes the warp an identity copy, so the only thing left
		// varying between the two runs is the decay arithmetic itself. The gate
		// still opens, because it measures the estimated motion before gain.
		params.motionGain      = 0.0f;

		// Build a field worth freezing, with decay off so this phase is
		// identical across both runs. Freeze stays off here: turning it on
		// before there is anything to hold just holds zero, and a zero field
		// closes the gate so nothing decays at all.
		params.motionSmoothing = 0.0f;
		params.decay        = 0.0f;
		float shift         = 0.0f;
		for( int frame = 0; frame < 8; ++frame )
		{
			rig.PushShifted( shift, 0.0f, params, frame );
			shift += 3.0f;
		}

		// One second of a constant white frame bleeding in. The field is frozen
		// now, so both runs displace by identical vectors and only the decay
		// arithmetic differs.
		params.motionSmoothing = 1.0f;
		// 0.45 under the geometric curve is a 0.85s half-life — what 0.9 used
		// to mean under the linear one. The test is about frame-rate
		// independence, not about a particular slider position, so it keeps
		// the half-life it was written against.
		params.decay        = 0.45f;
		params.deltaTime    = 1.0f / stepsPerSecond;
		const std::vector< uint8_t > white = MakeSolid( FRAME_WIDTH, FRAME_HEIGHT, 255, 255, 255 );
		for( int step = 0; step < stepsPerSecond; ++step )
			rig.PushImage( white, params, 8 + step );

		const float mean = MeanComponent( ReadTarget( rig.pipeline.GetAccumulation() ), 0 );
		rig.Teardown();
		return mean;
	};

	const float atThirty = meanAfterOneSecond( 30 );
	const float atSixty  = meanAfterOneSecond( 60 );

	CHECK( atThirty > 0.0f );
	CHECK( atSixty > 0.0f );
	// Partway there, so the test would actually notice a rate difference.
	CHECK( atThirty > 0.2f );
	CHECK( atThirty < 0.95f );
	CHECK_NEAR( atThirty, atSixty, 0.05 );
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

TEST( FeedbackBuffersStayFinite )
{
	Rig rig;
	CHECK( rig.Setup( 128, 96 ) );

	MoshParams params      = RawEstimatorParams();
	params.moshAmount      = 1.0f;
	params.motionThreshold = 0.0f;
	params.motionGain      = 4.0f;
	params.corruption      = 1.0f;
	params.chromaDrift     = 1.0f;
	params.decay           = 0.0f;
	params.motionSmoothing = 0.9f;

	// Deliberately hostile: full corruption at maximum gain, alternating between
	// structured content and flat frames the estimator cannot match. If anything
	// is going to produce a division by zero or an unbounded accumulation, it is
	// this. A NaN reaching either feedback buffer is unrecoverable in a live
	// show, so the bar is that none ever does.
	for( int frame = 0; frame < 40; ++frame )
	{
		if( frame % 4 == 3 )
			rig.PushImage( MakeSolid( 128, 96, 0, 0, 0 ), params, frame );
		else if( frame % 4 == 1 )
			rig.PushImage( MakeSolid( 128, 96, 255, 255, 255 ), params, frame );
		else
			rig.PushShifted( static_cast< float >( frame * 7 ), static_cast< float >( frame * 5 ), params, frame );
	}

	CHECK( AllFinite( ReadTarget( rig.pipeline.GetFlowField() ) ) );
	CHECK( AllFinite( ReadTarget( rig.pipeline.GetAccumulation() ) ) );
	CHECK( AllFinite( ReadTarget( rig.pipeline.GetControlState() ) ) );

	rig.Teardown();
}

TEST( SmallAndAwkwardResolutionsAreHandled )
{
	const int sizes[][ 2 ] = { { 1, 1 }, { 3, 7 }, { 17, 5 }, { 640, 1 }, { 33, 33 } };

	for( const auto& size : sizes )
	{
		Rig rig;
		CHECK( rig.Setup( size[ 0 ], size[ 1 ] ) );

		MoshParams params = RawEstimatorParams();
		params.moshAmount = 1.0f;
		for( int frame = 0; frame < 3; ++frame )
			rig.PushShifted( static_cast< float >( frame ), 0.0f, params, frame );

		CHECK( AllFinite( ReadTarget( rig.pipeline.GetAccumulation() ) ) );
		rig.Teardown();
	}
}

TEST( AllBlockSizesAndQualitiesRun )
{
	const int      blockSizes[] = { 4, 8, 16, 32 };
	const Quality  qualities[]  = { Quality::Low, Quality::Medium, Quality::High, Quality::Ultra };

	for( int blockSize : blockSizes )
	{
		for( Quality quality : qualities )
		{
			Rig rig;
			CHECK( rig.Setup( 160, 128 ) );

			MoshParams params = RawEstimatorParams();
			params.blockSize  = blockSize;
			params.quality    = quality;
			params.moshAmount = 1.0f;

			for( int frame = 0; frame < 5; ++frame )
				rig.PushShifted( static_cast< float >( frame * 2 ), 0.0f, params, frame );

			CHECK( AllFinite( ReadTarget( rig.pipeline.GetFlowField() ) ) );
			CHECK( rig.pipeline.GetFlowWidth() > 0 );
			rig.Teardown();
		}
	}
}

TEST( AdvanceRejectsUnusableInput )
{
	MoshPipeline pipeline;
	CHECK( pipeline.Initialise() );

	MoshParams params;
	FrameInputs empty;
	CHECK( !pipeline.Advance( empty, params ) );

	FrameInputs zeroSized;
	zeroSized.pixelTexture  = 1;
	zeroSized.motionTexture = 1;
	zeroSized.width         = 0;
	zeroSized.height        = 0;
	CHECK( !pipeline.Advance( zeroSized, params ) );

	pipeline.Release();
}

// ---------------------------------------------------------------------------
// Documented interactions
//
// These pin behaviours the parameter reference now promises. They are not bugs
// — each is the correct consequence of how the passes compose — but they are
// invisible from the parameter names, so a refactor could quietly change them
// and only the docs would be wrong. A failure here means site/parameters.html
// needs editing, not necessarily that the code does.
// ---------------------------------------------------------------------------

TEST( FullSmoothingMakesMotionLagInert )
{
	// Motion Lag reaches back through a 16-frame ring of vector fields. A full
	// hold — Motion Smoothing at 1, which is what Freeze used to be — stops the
	// ring updating, so every slot ends up holding the same vectors and each
	// lag setting selects an identical field. Documented under "when a knob
	// does nothing" — Bloom sets Smoothing to 1.0, so Lag is dead in Bloom.
	Rig rig;
	CHECK( rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) );

	MoshParams params    = RawEstimatorParams();
	params.moshAmount    = 1.0f;
	params.motionThreshold = 0.0f;

	// Real, varied motion first, so the ring genuinely holds different fields.
	float shift = 0.0f;
	int   frame = 0;
	for( ; frame < 12; ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += static_cast< float >( frame % 3 ) * 3.0f - 2.0f;
	}

	const std::vector< float > unfrozen0  = ReadTarget( rig.pipeline.GetDelayedFlowField( 0 ) );
	const std::vector< float > unfrozen15 = ReadTarget( rig.pipeline.GetDelayedFlowField( 15 ) );
	// Guard the premise: without freeze the ring must hold genuinely different
	// fields, or the assertion below would pass for the wrong reason.
	CHECK( MeanVectorDifference( unfrozen0, unfrozen15 ) > 1e-4f );

	// Now freeze, for longer than the ring is deep, with motion still happening.
	params.motionSmoothing = 1.0f;
	for( int i = 0; i < 20; ++i, ++frame )
	{
		rig.PushShifted( shift, 0.0f, params, frame );
		shift += static_cast< float >( i % 3 ) * 3.0f - 2.0f;
	}

	const std::vector< float > frozen0 = ReadTarget( rig.pipeline.GetDelayedFlowField( 0 ) );
	for( int lag = 1; lag <= 15; ++lag )
	{
		const std::vector< float > frozenN = ReadTarget( rig.pipeline.GetDelayedFlowField( lag ) );
		// Only .xy is held — the residual in .z is recomputed from the current
		// frame every pass, so comparing whole texels would fail for a reason
		// that has nothing to do with what this test is about.
		CHECK_NEAR( MeanVectorDifference( frozen0, frozenN ), 0.0, 1e-6 );
	}

	rig.Teardown();
}

TEST( FullMoshAmountMasksTheBurstControls )
{
	// The headline interaction, and the one that will waste the most time: the
	// gate is max( Mosh Amount + audio, held ), so at Mosh Amount 1.0 the burst
	// machinery cannot raise a level that is already at the ceiling. Trigger,
	// Hold, Auto Mode, Cut Sensitivity, Burst Length and Beat Divisor all go
	// inert together. Every Style preset used to set it there; none does now,
	// and that is why.
	auto levelAfter = []( float moshAmount, bool trigger ) {
		Rig rig;
		if( !rig.Setup( 128, 128 ) )
			return -1.0f;

		MoshParams params  = RawEstimatorParams();
		params.moshAmount  = moshAmount;
		params.duration    = 1.0f;
		params.deltaTime   = 1.0f / 60.0f;

		rig.PushShifted( 0.0f, 0.0f, params, 0 );
		rig.PushShifted( 2.0f, 0.0f, params, 1 );

		params.trigger = trigger;
		rig.PushShifted( 4.0f, 0.0f, params, 2 );

		const float level = ReadTarget( rig.pipeline.GetControlState() )[ 0 ];
		rig.Teardown();
		return level;
	};

	// At full Mosh Amount the trigger changes nothing — it is already at 1.
	CHECK_NEAR( levelAfter( 1.0f, false ), 1.0, 1e-3 );
	CHECK_NEAR( levelAfter( 1.0f, true ),  1.0, 1e-3 );

	// Below full it very much does, which is what makes the above a masking
	// result rather than a trigger that never worked.
	CHECK( levelAfter( 0.0f, true ) - levelAfter( 0.0f, false ) > 0.9f );
}

TEST( DamageSpreadsIntoBlocksThatNeverMoved )
{
	// The only evidence this feature works: the PICTURE changing in a block with
	// no motion of its own. Everything else the plugin does is affine — decided
	// from this frame's numbers alone — so a block that never moved could never
	// mosh. This is the one pass with a spatial memory.
	//
	// Measuring the damage field instead would prove nothing. A field can be
	// perfectly populated while the mosh pass ignores it entirely, and the first
	// version of this test passed with exactly that bug reintroduced.
	//
	// A band down the left third moves; the rest is the same pattern standing
	// still, so it has no motion of its own by construction. That still region
	// also BRIGHTENS over the run, because a region whose content never changes
	// cannot show whether it moshed — holding a pixel and refreshing it give the
	// same colour when the pixel never changes.
	constexpr int W = 256, H = 192;

	auto render = []( float spread, float moshAmount ) {
		Rig rig;
		if( !rig.Setup( W, H ) )
			return std::vector< float >();

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = moshAmount;
		params.motionThreshold = 0.0f;
		params.spread          = spread;

		for( int frame = 0; frame < 30; ++frame )
		{
			const float seconds = frame / 60.0f;
			rig.texture.Upload( MakeMovingBandPattern( W, H, seconds * 180.0f, 0.0f,
			                                           0.33f, seconds * 0.6f ) );
			params.frame = frame;
			rig.pipeline.Advance( rig.Inputs(), params );
		}
		std::vector< float > out = ReadTarget( rig.pipeline.GetAccumulation() );
		rig.Teardown();
		return out;
	};

	// Mosh Amount 0 is an exact passthrough, so this is "how much did the effect
	// do" — measured well outside the moving band, and inside the creep's reach.
	const std::vector< float > reference = render( 0.0f, 0.0f );
	auto stillRegion = [ & ]( float spread ) {
		const std::vector< float > out = render( spread, 1.0f );
		double                     total = 0.0;
		int                        count = 0;
		for( int y = 8; y < H - 8; ++y )
		{
			for( int x = static_cast< int >( W * 0.40f ); x < static_cast< int >( W * 0.60f ); ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * W + x ) * 4;
				total += std::fabs( out[ i ] - reference[ i ] );
				++count;
			}
		}
		return count == 0 ? -1.0f : static_cast< float >( total / count );
	};

	// Spread 0 leaves the still region untouched — that is what makes this not a
	// breaking change, and the pass runs every frame regardless.
	const float off = stillRegion( 0.0f );
	CHECK( off < 0.005f );

	// Turned up, the creep reaches it and the region stops refreshing.
	const float low  = stillRegion( 0.4f );
	const float mid  = stillRegion( 0.7f );
	const float high = stillRegion( 1.0f );
	CHECK( mid > 0.03f );
	// And further is further: the control has to have a range, not a switch.
	CHECK( low > off );
	CHECK( mid > low );
	CHECK( high > mid );
}

TEST( SpreadReachesFurtherAcrossItsWholeTravel )
{
	// The reach that matters is where the field is still strong enough to open
	// the gate, and that is LINEAR in the propagation speed while the slider is
	// logarithmic in it. Shipped in 0.3.0, that put the entire usable creep in
	// the top of the travel — at 16:9 and Block Size 16, Spread 0.25 reached 0
	// blocks past the seed, 0.50 reached 1, 0.75 reached 2, and only 1.00 did
	// anything at 5. Three quarters of the fader was inert.
	//
	// That is the fault 0.2.0 was named for removing, reintroduced in a control
	// added after it. No test caught it because every existing one asked "does
	// Spread do something" at a single setting, and at the top of the range it
	// always did.
	constexpr int W = 640, H = 360;

	auto reachBlocks = []( float spread ) {
		Rig rig;
		if( !rig.Setup( W, H ) )
			return -1;

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = 0.8f;
		params.motionThreshold = 0.0f;
		params.motionSmoothing = 0.3f;
		params.spread          = spread;

		// A full second, so the field reaches its settled extent rather than
		// wherever it happened to have got to.
		for( int frame = 0; frame < 60; ++frame )
		{
			rig.texture.Upload( MakeMovingBandPattern( W, H, frame * 3.0f, 0.0f, 0.25f, 0.0f ) );
			params.frame = frame;
			rig.pipeline.Advance( rig.Inputs(), params );
		}

		const RenderTarget&        field = rig.pipeline.GetDamageField();
		const int                  fw    = field.GetWidth();
		const int                  fh    = field.GetHeight();
		const std::vector< float > data  = ReadTarget( field );

		// Furthest block whose damage still opens the gate substantially —
		// the same 0.35 MoshDamageOpening reaches full on.
		int furthest = 0;
		for( int y = 0; y < fh; ++y )
			for( int x = 0; x < fw; ++x )
				if( data[ ( static_cast< size_t >( y ) * fw + x ) * 4 ] > 0.35f && x > furthest )
					furthest = x;

		rig.Teardown();
		// Past the seeding band, which ends a quarter of the way across.
		return furthest - static_cast< int >( fw * 0.25f );
	};

	const int quarter = reachBlocks( 0.25f );
	const int half    = reachBlocks( 0.50f );
	const int threeQ  = reachBlocks( 0.75f );
	const int full    = reachBlocks( 1.00f );

	// Every quarter turn must buy meaningfully more reach than the last. A
	// control whose travel is monotone but flat until the end is the thing this
	// test exists to reject, so these are gaps, not just ordering.
	CHECK( quarter >= 1 );
	CHECK( half >= quarter + 2 );
	CHECK( threeQ >= half + 4 );
	CHECK( full >= threeQ + 8 );

	// And the top reaches a serious fraction of the frame, or "spread" is a
	// halo rather than a spread. Measured 22 blocks of 40, 55% of the width.
	CHECK( full > 15 );
}

TEST( DamageCreepIsIndependentOfFrameRate )
{
	// The trap this feature has, and the reason the spread is a dilation rather
	// than a blur. A neighbourhood max advances the front by exactly its radius
	// per pass, so a radius of speed*dt makes the distance covered depend only
	// on wall-clock time — radii compose by addition. A mean blur is diffusion:
	// its front advances as the square root of elapsed time and no coefficient
	// recovers frame-rate independence.
	//
	// The still region does NOT brighten here, unlike the test above. A ramping
	// brightness makes the block matcher find spurious motion, and it finds more
	// of it at the coarser frame step — which seeds damage directly across the
	// whole frame at 30fps and none of it at 60. That measures the test image,
	// not the dilation, and it is what the first version of this test did.
	constexpr int W = 512, H = 256;

	auto frontAfterHalfASecond = []( int stepsPerSecond ) {
		Rig rig;
		if( !rig.Setup( W, H ) )
			return -1.0f;

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = 1.0f;
		params.motionThreshold = 0.0f;
		params.spread          = 0.7f;
		params.deltaTime       = 1.0f / stepsPerSecond;

		const int steps = stepsPerSecond / 2;
		for( int frame = 0; frame < steps; ++frame )
		{
			// The same wall-clock motion either way, so the seed is identical
			// and only the number of dilation passes differs.
			const float seconds = static_cast< float >( frame ) / stepsPerSecond;
			rig.texture.Upload( MakeMovingBandPattern( W, H, seconds * 180.0f, 0.0f,
			                                           0.33f, 0.0f ) );
			params.frame = frame;
			rig.pipeline.Advance( rig.Inputs(), params );
		}

		const RenderTarget&        field = rig.pipeline.GetDamageField();
		const int                  fw    = field.GetWidth();
		const int                  fh    = field.GetHeight();
		const std::vector< float > data  = ReadTarget( field );

		// How far right the front got, as a fraction of the width.
		int furthest = 0;
		for( int y = 0; y < fh; ++y )
			for( int x = 0; x < fw; ++x )
				if( data[ ( static_cast< size_t >( y ) * fw + x ) * 4 ] > 0.05f && x > furthest )
					furthest = x;

		rig.Teardown();
		return static_cast< float >( furthest ) / static_cast< float >( fw );
	};

	const float atThirty = frontAfterHalfASecond( 30 );
	const float atSixty  = frontAfterHalfASecond( 60 );

	// Partway across, or this compares two fronts saturated against the frame
	// edge, which would agree however wrong the arithmetic was. That is exactly
	// how the first version of this test passed with the radius fixed per frame.
	CHECK( atThirty > 0.40f );
	CHECK( atThirty < 0.90f );
	CHECK( atSixty > 0.40f );
	CHECK( atSixty < 0.90f );
	// Measured 0.500 against 0.531 — one block of 32 apart.
	CHECK_NEAR( atThirty, atSixty, 0.08 );
}

TEST( ResetClearsTheDamageField )
{
	// Reset is a keyframe. The creep is spatial memory, so it has to go the way
	// the vector field does — a damage front grown before a cut surviving it is
	// exactly what Reset exists to abolish.
	//
	// Measured OUTSIDE the moving band, where only the creep can have put
	// anything. Inside the band the frame after a Reset re-seeds immediately
	// from live motion, so a whole-field mean stays high and asserting on it
	// tests nothing — which is how the first version of this passed with the
	// clear removed.
	constexpr int W = 256, H = 192;

	Rig rig;
	CHECK( rig.Setup( W, H ) );

	MoshParams params      = RawEstimatorParams();
	params.moshAmount      = 1.0f;
	params.motionThreshold = 0.0f;
	params.spread          = 1.0f;

	auto push = [ & ]( int frame ) {
		rig.texture.Upload( MakeMovingBandPattern( W, H, frame * 3.0f, 0.0f, 0.33f, 0.0f ) );
		params.frame = frame;
		rig.pipeline.Advance( rig.Inputs(), params );
	};

	// Damage beyond the seeding band, which only the creep can reach.
	auto beyondTheBand = [ & ]() {
		const RenderTarget&        field = rig.pipeline.GetDamageField();
		const int                  fw    = field.GetWidth();
		const int                  fh    = field.GetHeight();
		const std::vector< float > data  = ReadTarget( field );
		double                     total = 0.0;
		int                        count = 0;
		for( int y = 0; y < fh; ++y )
		{
			for( int x = fw / 2; x < fw; ++x )
			{
				total += data[ ( static_cast< size_t >( y ) * fw + x ) * 4 ];
				++count;
			}
		}
		return count == 0 ? -1.0f : static_cast< float >( total / count );
	};

	int frame = 0;
	for( ; frame < 30; ++frame )
		push( frame );

	const float grown = beyondTheBand();
	CHECK( grown > 0.05f );

	params.reset = true;
	push( frame++ );
	params.reset = false;

	// One frame after the keyframe the creep has not had time to leave the band,
	// so beyond it there must be essentially nothing.
	CHECK( beyondTheBand() < 0.1f * grown );
}

TEST( LumaMaskLocalisesTheMoshAndInvertSwapsIt )
{
	// The mask reads the brightness of the MOTION input and decides where the
	// mosh is allowed to stick. Both halves carry the same contrast and differ
	// only in DC offset, so the estimator sees identical structure on both
	// sides and anything that differs across the split can only be the mask.
	//
	// Each half is compared against ITSELF across mask settings, never against
	// the other half. The two halves sample different regions of the noise
	// field, so they mosh by different amounts even with the mask off — 0.018
	// against 0.004 — and a test that expected them equal would be asserting
	// something untrue about the pattern rather than anything about the mask.
	//
	// Every configuration also has to show moshing happening SOMEWHERE. A mask
	// reading zero gives persistence 0, which is a pixel-exact copy of the
	// input and indistinguishable from a dead plugin, so "this half is clean"
	// can never be the whole of a passing result.
	constexpr int W = 160, H = 128;

	struct Halves { float bright, dark; };
	auto run = [ & ]( float maskAmount, bool invert ) {
		std::vector< float > arm[ 2 ];
		for( int i = 0; i < 2; ++i )
		{
			Rig rig;
			if( !rig.Setup( W, H ) )
				return Halves{ -1.0f, -1.0f };

			MoshParams params      = RawEstimatorParams();
			params.moshAmount      = ( i == 0 ) ? 0.0f : 0.9f;
			params.motionThreshold = 0.0f;
			params.maskAmount      = maskAmount;
			params.maskInvert      = invert;

			for( int frame = 0; frame < 12; ++frame )
			{
				// Bright on the left throughout. Amplitude 0.30 on both sides:
				// below about that the block matcher stops finding motion at
				// all and the test measures the estimator instead.
				rig.texture.Upload( MakeSplitPattern( W, H, frame * 3.0f, 0.0f,
				                                      0.68f, 0.02f, 0.30f, true ) );
				params.frame = frame;
				rig.pipeline.Advance( rig.Inputs(), params );
			}
			arm[ i ] = ReadTarget( rig.pipeline.GetAccumulation() );
			rig.Teardown();
		}
		// Distance from the same run at Mosh Amount 0, which is an exact
		// passthrough — so this is "how much did the effect do", per half.
		return Halves{ HalfDifference( arm[ 0 ], arm[ 1 ], W, H, 12, true ),
		               HalfDifference( arm[ 0 ], arm[ 1 ], W, H, 12, false ) };
	};

	// The control. Both halves mosh; without this a mask that simply killed
	// everything would satisfy every suppression check below.
	const Halves off = run( 0.0f, false );
	CHECK( off.bright > 0.008f );
	CHECK( off.dark > 0.002f );

	// Masked: the bright half still moshes, the dark half is suppressed
	// against what that same half did unmasked.
	const Halves on = run( 1.0f, false );
	CHECK( on.bright > 0.004f );
	CHECK( on.dark < 0.25f * off.dark );

	// Inverted: the same split, the other way round. The image is unchanged —
	// only where the mosh lands moves.
	const Halves flipped = run( 1.0f, true );
	CHECK( flipped.dark > 0.001f );
	CHECK( flipped.bright < 0.25f * off.bright );

	// Half depth lands between the two, rather than snapping to one of them.
	// That gradient is the whole reason the mask multiplies the gate instead of
	// scaling the hold time, where a half-lit pixel and a fully lit one would
	// both read as simply "held".
	const Halves half = run( 0.5f, false );
	CHECK( half.dark > on.dark );
	CHECK( half.dark < off.dark );
}

TEST( LumaMaskUsesThisFramesLumaNotLastFrames )
{
	// PassLuma renders into luma.Back() and luma.Swap() runs AFTER PassMosh, so
	// during the mosh pass Back() is this frame's luma and Front() is the
	// previous frame's. Binding the wrong one is a one-word mistake that leaves
	// the mask working perfectly on any footage where the mask does not move —
	// which is every test built on a static split, including the one above.
	//
	// So: establish a mosh with the bright half on the left, then push ONE
	// frame with the split flipped. A mask reading this frame protects the
	// left; a mask reading last frame still protects the right.
	constexpr int W = 160, H = 128;

	std::vector< float > arm[ 2 ];
	for( int i = 0; i < 2; ++i )
	{
		Rig rig;
		CHECK( rig.Setup( W, H ) );

		MoshParams params      = RawEstimatorParams();
		params.moshAmount      = ( i == 0 ) ? 0.0f : 0.9f;
		params.motionThreshold = 0.0f;
		params.maskAmount      = 1.0f;

		int frame = 0;
		for( ; frame < 12; ++frame )
		{
			rig.texture.Upload( MakeSplitPattern( W, H, frame * 3.0f, 0.0f,
			                                      0.68f, 0.02f, 0.30f, true ) );
			params.frame = frame;
			rig.pipeline.Advance( rig.Inputs(), params );
		}
		// One frame with the bright half on the RIGHT.
		rig.texture.Upload( MakeSplitPattern( W, H, frame * 3.0f, 0.0f,
		                                      0.68f, 0.02f, 0.30f, false ) );
		params.frame = frame;
		rig.pipeline.Advance( rig.Inputs(), params );

		arm[ i ] = ReadTarget( rig.pipeline.GetAccumulation() );
		rig.Teardown();
	}

	const float left  = HalfDifference( arm[ 0 ], arm[ 1 ], W, H, 12, true );
	const float right = HalfDifference( arm[ 0 ], arm[ 1 ], W, H, 12, false );

	// The bright half is now on the right, so that is the side still allowed to
	// hold. Reading last frame's luma gives the mirror image of this.
	CHECK( right > 0.001f );
	CHECK( left < 0.5f * right );
}

TEST( GateIsIndependentOfFrameRate )
{
	// The gate is a spatial term multiplied into `persistence`, and persistence
	// is the pole of a feedback loop rather than an output weight: the mosh
	// pass writes into the accumulation buffer, and next frame reads it back as
	// AccumPrev. So a raw multiplier there is a RATE. Mosh Amount and Decay are
	// built as exp2(-dt/T) so that applying them f times a second cancels dt;
	// the gate was a plain multiply, so survival over a wall-clock interval was
	// gate^4 against gate^8 for the same interval at half the frame rate.
	//
	// Same shape as DecayIsIndependentOfFrameRate, and for the same reason.
	// Mosh Amount 1.0 and Decay 0 pin both time curves at exactly 1, so the
	// gate is the only thing left varying between the two runs.
	const auto meanAfterAnInterval = []( int stepsPerSecond, int steps ) {
		Rig rig;
		if( !rig.Setup( FRAME_WIDTH, FRAME_HEIGHT ) )
			return -1.0f;

		MoshParams params = RawEstimatorParams();
		params.moshAmount = 1.0f;
		params.decay      = 0.0f;
		// Identity warp, so both runs displace by the same nothing.
		params.motionGain = 0.0f;
		params.softness   = 1.0f;
		// Just under the ~3px/frame the warm-up builds, so smoothstep lands
		// part-open rather than hard against either stop. The window either
		// side of this is narrow: the gate is a smoothstep over half a pixel of
		// motion, so intermediate values are genuinely hard to reach — which is
		// exactly why this bug survived the release that was named for fixing
		// units. It was real everywhere and visible almost nowhere.
		params.motionThreshold = 0.57f;

		params.motionSmoothing = 0.0f;
		float shift            = 0.0f;
		for( int frame = 0; frame < 8; ++frame )
		{
			rig.PushShifted( shift, 0.0f, params, frame );
			shift += 3.0f;
		}

		// Field frozen, so the gate is fixed per pixel from here and a white
		// frame bleeds in through it at whatever rate it allows.
		params.motionSmoothing             = 1.0f;
		params.deltaTime                   = 1.0f / stepsPerSecond;
		const std::vector< uint8_t > white = MakeSolid( FRAME_WIDTH, FRAME_HEIGHT, 255, 255, 255 );
		for( int step = 0; step < steps; ++step )
			rig.PushImage( white, params, 8 + step );

		const float mean = MeanComponent( ReadTarget( rig.pipeline.GetAccumulation() ), 0 );
		rig.Teardown();
		return mean;
	};

	// The same 0.133s, delivered as four steps or as eight.
	const float atThirty = meanAfterAnInterval( 30, 4 );
	const float atSixty  = meanAfterAnInterval( 60, 8 );

	// Both must be partway between the two stops, or this is comparing
	// saturated results and would hold however wrong the arithmetic was. A
	// fully held frame measures 0.56 here and a fully refreshed one 0.9995.
	CHECK( atThirty > 0.62f );
	CHECK( atThirty < 0.97f );
	CHECK( atSixty > 0.62f );
	CHECK( atSixty < 0.97f );

	// Measured 0.0005 apart normalised, and 0.108 apart without it.
	CHECK_NEAR( atThirty, atSixty, 0.02 );
}

TEST( QuantiseDoesNotSilenceSlowMotion )
{
	// Quantise used to be applied to the stored vector field, upstream of the
	// motion gate. Any motion below half a grid step therefore rounded to zero,
	// zero motion closed the gate, and a closed gate refreshes the block from
	// the live frame — so the plugin rendered a pixel-exact passthrough exactly
	// where it had been asked to damage the image hardest.
	//
	// The grid step reaches half the macroblock, so at the default block size
	// of 16 the dead zone ran to 3 px/frame, against this file's own note that
	// per-frame motion on ordinary footage is mostly 0.5-3 px. The shipped
	// Corrupt preset sets Quantise 0.7 and was inside it.
	//
	// Slow motion is the whole point of the test. At 6 px/frame every Quantise
	// setting always worked, which is why nothing caught this.
	auto departure = []( float pxPerFrame, float quantise ) {
		std::vector< float > arm[ 2 ];
		for( int i = 0; i < 2; ++i )
		{
			Rig rig;
			if( !rig.Setup( 160, 128 ) )
				return -1.0f;

			MoshParams params      = RawEstimatorParams();
			params.moshAmount      = ( i == 0 ) ? 0.0f : 0.8f;
			params.motionQuantise  = quantise;
			params.motionThreshold = 0.0f;
			params.motionSmoothing = 0.3f;
			params.softness        = 0.15f;

			for( int frame = 0; frame < 24; ++frame )
				rig.PushShifted( frame * pxPerFrame, 0.0f, params, frame );

			arm[ i ] = ReadTarget( rig.pipeline.GetAccumulation() );
			rig.Teardown();
		}
		return MeanInteriorDifference( arm[ 0 ], arm[ 1 ], 160, 128, 16 );
	};

	// Mosh Amount 0 is an exact passthrough, so this is distance from "nothing
	// happened". Anything at or near zero means the effect is not running.
	const float unquantised = departure( 1.5f, 0.0f );
	CHECK( unquantised > 0.02f );

	// Every Quantise setting, at motion far inside the old dead zone. Before
	// the fix, 0.4 upwards measured 0.0000 here.
	for( float quantise : { 0.4f, 0.5f, 0.7f, 0.85f, 1.0f } )
		CHECK( departure( 1.5f, quantise ) > 0.02f );
}

}  // namespace datamosh::test
