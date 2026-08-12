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
	params.motionFreeze    = 0.0f;
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

/// Mean absolute difference over the interior of two full-resolution buffers.
///
/// The frame border is excluded because content there was off-screen a frame
/// ago: no estimator can reconstruct it, and every warp smears it inward.
float MeanInteriorDifference( const std::vector< float >& a, const std::vector< float >& b,
                              int width, int height, int margin )
{
	if( a.size() != b.size() || a.empty() )
		return -1.0f;
	double total = 0.0;
	int    count = 0;
	for( int y = margin; y < height - margin; ++y )
	{
		for( int x = margin; x < width - margin; ++x )
		{
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			for( int channel = 0; channel < 3; ++channel )
				total += std::fabs( a[ index + channel ] - b[ index + channel ] );
			count += 3;
		}
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

TEST( FreezeHoldsTheVectorField )
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
	params.motionFreeze = 1.0f;
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
		params.motionFreeze = freeze;

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

}  // namespace datamosh::test
