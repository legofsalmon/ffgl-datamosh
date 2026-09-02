// Tests for the plugin layer — the FFGL-facing shell around the render core.
//
// test_pipeline.cpp drives MoshPipeline directly and never touches any of this.
// That gap mattered: of the five defects the first review found, four lived
// here, in the frame gate, the trigger handling, the parameter plumbing and the
// mixer's input selection. These cover that surface.

#include "harness/Synthetic.h"
#include "harness/TestRunner.h"

#include <DatamoshEffect.h>
#include <DatamoshMixer.h>
#include <MoshParams.h>
#include <RenderTarget.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace datamosh::test {

namespace {

constexpr int FRAME_WIDTH  = 128;
constexpr int FRAME_HEIGHT = 96;

/// Exposes the protected surface a test needs to observe.
template< typename PluginType >
struct Testable : PluginType
{
	using PluginType::frameCounter;
	using PluginType::lastAppliedDeltaTime;
	using PluginType::NO_PARAM;
	using PluginType::ParamIndex;
	using PluginType::audioParam;
	using PluginType::AudioLevel;
	using PluginType::pipeline;
	using PluginType::ReadParams;
	using PluginType::UpdateAudioAndTime;
	using PluginType::ParamValue;

	/// How many parameters the plugin actually holds, as against how many it
	/// advertises to the host. The two diverging is what makes a plugin
	/// unloadable.
	size_t ParamCount() const { return this->params.size(); }
};

using TestableEffect = Testable< DatamoshEffect >;
using TestableMixer  = Testable< DatamoshMixer >;

/// Stands in for the host: owns the input textures and the target framebuffer,
/// and hands the plugin the ProcessOpenGLStruct Resolume would.
class Host
{
public:
	bool Setup( int width, int height, int inputCount )
	{
		this->width  = width;
		this->height = height;
		count        = inputCount;

		// A surfaceless context has no default framebuffer, so the plugin needs
		// somewhere real to composite into.
		if( !output.Allocate( width, height, GL_RGBA8 ) )
			return false;

		for( int index = 0; index < inputCount; ++index )
		{
			if( !textures[ index ].Create( width, height ) )
				return false;
			descriptors[ index ].Width          = static_cast< FFUInt32 >( width );
			descriptors[ index ].Height         = static_cast< FFUInt32 >( height );
			descriptors[ index ].HardwareWidth  = static_cast< FFUInt32 >( width );
			descriptors[ index ].HardwareHeight = static_cast< FFUInt32 >( height );
			descriptors[ index ].Handle         = textures[ index ].GetHandle();
			pointers[ index ]                   = &descriptors[ index ];
		}

		glViewport( 0, 0, width, height );
		return true;
	}

	/// Gives one input a different size from the other, for the mismatch test.
	bool ResizeInput( int index, int newWidth, int newHeight )
	{
		if( !textures[ index ].Create( newWidth, newHeight ) )
			return false;
		descriptors[ index ].Width          = static_cast< FFUInt32 >( newWidth );
		descriptors[ index ].Height         = static_cast< FFUInt32 >( newHeight );
		descriptors[ index ].HardwareWidth  = static_cast< FFUInt32 >( newWidth );
		descriptors[ index ].HardwareHeight = static_cast< FFUInt32 >( newHeight );
		descriptors[ index ].Handle         = textures[ index ].GetHandle();
		return true;
	}

	void Fill( int index, float shiftX, float shiftY )
	{
		textures[ index ].Upload( MakeShiftedPattern( textures[ index ].GetWidth(),
		                                              textures[ index ].GetHeight(),
		                                              shiftX, shiftY ) );
	}

	ProcessOpenGLStruct Frame()
	{
		ProcessOpenGLStruct pGL{};
		pGL.numInputTextures = static_cast< FFUInt32 >( count );
		pGL.inputTextures    = pointers;
		pGL.HostFBO          = output.GetFBO();
		return pGL;
	}

	FFGLViewportStruct Viewport() const
	{
		FFGLViewportStruct viewport{};
		viewport.x      = 0;
		viewport.y      = 0;
		viewport.width  = static_cast< GLuint >( width );
		viewport.height = static_cast< GLuint >( height );
		return viewport;
	}

	void Teardown()
	{
		for( InputTexture& texture : textures )
			texture.Release();
		output.Release();
	}

private:
	InputTexture       textures[ 2 ];
	FFGLTextureStruct  descriptors[ 2 ]{};
	FFGLTextureStruct* pointers[ 2 ]{};
	RenderTarget       output;
	int                width  = 0;
	int                height = 0;
	int                count  = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST( PluginInitialisesRendersAndReleases )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	host.Fill( 0, 0.0f, 0.0f );
	ProcessOpenGLStruct frame = host.Frame();
	CHECK( plugin.ProcessOpenGL( &frame ) == FF_SUCCESS );

	CHECK( plugin.DeInitGL() == FF_SUCCESS );
	host.Teardown();
}

TEST( BadInputIsRejectedWithoutCrashing )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	CHECK( plugin.ProcessOpenGL( nullptr ) == FF_FAIL );

	ProcessOpenGLStruct noTextures{};
	noTextures.numInputTextures = 0;
	noTextures.inputTextures    = nullptr;
	CHECK( plugin.ProcessOpenGL( &noTextures ) == FF_FAIL );

	FFGLTextureStruct* nullPointer[ 1 ] = { nullptr };
	ProcessOpenGLStruct nullTexture{};
	nullTexture.numInputTextures = 1;
	nullTexture.inputTextures    = nullPointer;
	CHECK( plugin.ProcessOpenGL( &nullTexture ) == FF_FAIL );

	// Nothing should have been advanced by any of that.
	CHECK( plugin.frameCounter == 0 );

	plugin.DeInitGL();
	host.Teardown();
}

// ---------------------------------------------------------------------------
// The frame-advance gate
// ---------------------------------------------------------------------------

TEST( FrameGateAdvancesOncePerHostTime )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	// A host can call a plugin more than once for the same moment. A feedback
	// effect that advanced on every call would run at a speed that depended on
	// how the composition happened to be wired.
	plugin.SetTime( 1.0 );
	for( int call = 0; call < 3; ++call )
	{
		host.Fill( 0, static_cast< float >( call ), 0.0f );
		ProcessOpenGLStruct frame = host.Frame();
		plugin.ProcessOpenGL( &frame );
	}
	CHECK( plugin.frameCounter == 1 );

	plugin.SetTime( 2.0 );
	ProcessOpenGLStruct frame = host.Frame();
	plugin.ProcessOpenGL( &frame );
	CHECK( plugin.frameCounter == 2 );

	plugin.DeInitGL();
	host.Teardown();
}

TEST( WithoutHostTimeEveryCallAdvances )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	// SetTime is deliberately never called. If a host does not deliver time, the
	// gate has to fall open: a wrong rate is recoverable, an effect frozen for
	// the whole show is not.
	for( int call = 0; call < 4; ++call )
	{
		host.Fill( 0, static_cast< float >( call ), 0.0f );
		ProcessOpenGLStruct frame = host.Frame();
		plugin.ProcessOpenGL( &frame );
	}
	CHECK( plugin.frameCounter == 4 );

	plugin.DeInitGL();
	host.Teardown();
}

TEST( TimeBankedByGatedCallsIsNotLost )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	plugin.SetTime( 1.0 );
	ProcessOpenGLStruct frame = host.Frame();
	plugin.ProcessOpenGL( &frame );

	const float firstDelta = plugin.lastAppliedDeltaTime;
	CHECK( firstDelta > 0.0f );

	// Four calls at one host time: one advances, three are gated out. The time
	// that passed during the gated three still passed, and a burst measured in
	// seconds has to see it.
	for( int call = 0; call < 3; ++call )
		plugin.ProcessOpenGL( &frame );

	plugin.SetTime( 2.0 );
	plugin.ProcessOpenGL( &frame );

	// Each call banks at least the floor of 1/240s, so four of them cannot be
	// accounted for by one.
	CHECK( plugin.lastAppliedDeltaTime >= 4.0f / 240.0f );

	plugin.DeInitGL();
	host.Teardown();
}

TEST( HoldIsNotConsumedTheWayATriggerIs )
{
	// Hold has to survive what Trigger deliberately does not. consumeAllTrigger()
	// zeroes every ParamTrigger after each rendered frame, which is exactly why
	// hold is a ParamBool: an event cannot express a state that outlives a
	// frame. If someone "tidies" it into a ParamTrigger, this fails on frame two.
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	const unsigned int holdIndex = plugin.ParamIndex( "Hold" );
	CHECK( holdIndex != TestableEffect::NO_PARAM );

	// Through the host's setter, once, as a host would on a key-down.
	plugin.SetFloatParameter( holdIndex, 1.0f );

	for( int frame = 1; frame <= 5; ++frame )
	{
		plugin.SetTime( frame );
		ProcessOpenGLStruct pGL = host.Frame();
		plugin.ProcessOpenGL( &pGL );
		CHECK( plugin.ReadParams().hold );
	}

	// And it must clear on the way back down — a hold that cannot be released
	// is worse than one that never engaged.
	plugin.SetFloatParameter( holdIndex, 0.0f );
	plugin.SetTime( 6.0 );
	ProcessOpenGLStruct pGL = host.Frame();
	plugin.ProcessOpenGL( &pGL );
	CHECK( !plugin.ReadParams().hold );

	plugin.DeInitGL();
	host.Teardown();
}

TEST( ResetEndsAHoldWhoseReleaseNeverArrived )
{
	// The recovery that matters. Hold is the only state in this plugin that
	// cannot expire on its own, so if a key-up is lost — focus moved to another
	// application mid-hold, a dropped MIDI note-off — the operator is stranded
	// with the output destroyed. Reset has to end it while the parameter is
	// still reading true, and the shader alone cannot do that: it zeroes the
	// level, and the still-true hold puts it straight back the next frame.
	TestableEffect plugin;

	const unsigned int holdIndex  = plugin.ParamIndex( "Hold" );
	const unsigned int resetIndex = plugin.ParamIndex( "Reset" );
	CHECK( holdIndex != TestableEffect::NO_PARAM );
	CHECK( resetIndex != TestableEffect::NO_PARAM );

	plugin.SetFloatParameter( holdIndex, 1.0f );
	CHECK( plugin.ReadParams().hold );

	// Reset, with the hold still physically down.
	plugin.SetFloatParameter( resetIndex, 1.0f );
	CHECK( !plugin.ReadParams().hold );

	// And it stays ended for as long as the stuck hold reads true, rather than
	// blinking off for a frame and coming back.
	plugin.SetFloatParameter( resetIndex, 0.0f );
	for( int frame = 0; frame < 5; ++frame )
		CHECK( !plugin.ReadParams().hold );

	// A genuine release re-arms it, so the next press works normally.
	plugin.SetFloatParameter( holdIndex, 0.0f );
	CHECK( !plugin.ReadParams().hold );
	plugin.SetFloatParameter( holdIndex, 1.0f );
	CHECK( plugin.ReadParams().hold );
}


TEST( SlidersStillReachThePipelineWhileStyleShowsCustom )
{
	// Custom is a label, not a mode. It means "these values match no preset",
	// and selecting it deliberately changes nothing — so parameters must keep
	// working exactly as before. Written after a report that the effect could
	// not be activated on Custom; the cause turned out to be leftover values,
	// not broken plumbing, and this pins the plumbing so the next such report
	// can be triaged in one step.
	TestableEffect plugin;
	const unsigned int styleIdx = plugin.ParamIndex( "Style" );
	const unsigned int moshIdx  = plugin.ParamIndex( "Mosh Amount" );

	// From the default Custom, a slider write reaches ReadParams. The default
	// is 0.3 now — a fresh instance visibly moshes rather than passing through.
	CHECK_NEAR( plugin.ReadParams().moshAmount, 0.3, 1e-4 );
	plugin.SetFloatParameter( moshIdx, 1.0f );
	CHECK_NEAR( plugin.ReadParams().moshAmount, 1.0, 1e-4 );

	// And after a style has been selected and then abandoned.
	plugin.SetFloatParameter( styleIdx, static_cast< float >( Style::Melt ) );
	plugin.SetFloatParameter( styleIdx, static_cast< float >( Style::Custom ) );
	plugin.SetFloatParameter( moshIdx, 0.7f );
	CHECK_NEAR( plugin.ReadParams().moshAmount, 0.7, 1e-4 );
}

TEST( AbandoningAStyleKeepsItsValues )
{
	// The trap behind that report. Selecting Custom does not restore defaults,
	// so a style's Motion Threshold survives it — and Drag leaves 0.5. Under the
	// squared law that is 2px of motion per frame rather than the 4px that shut
	// the gate frame-wide on ordinary footage, and the Default entry in the
	// Style list is the way back. The persistence itself is by design: Custom
	// means "edited", and restoring on it would wipe the operator's own edits.
	TestableEffect plugin;
	const unsigned int styleIdx = plugin.ParamIndex( "Style" );

	const float defaultThreshold = plugin.ReadParams().motionThreshold;
	CHECK_NEAR( defaultThreshold, 0.15, 1e-4 );

	plugin.SetFloatParameter( styleIdx, static_cast< float >( Style::Drag ) );
	CHECK_NEAR( plugin.ReadParams().motionThreshold, 0.5, 1e-4 );

	plugin.SetFloatParameter( styleIdx, static_cast< float >( Style::Custom ) );
	// Still 0.5, not back to the 0.15 default — this is the documented
	// behaviour, and the reason the docs tell you to check it.
	CHECK_NEAR( plugin.ReadParams().motionThreshold, 0.5, 1e-4 );
	// Mosh Amount is untouched by every preset — it is the performance
	// control, and a preset never writes the performance control — so it
	// still reads the factory default. That used to be the other half of this
	// trap: presets left it at 1.0, where raising it could not help either.
	CHECK_NEAR( plugin.ReadParams().moshAmount, 0.3, 1e-4 );
}

TEST( TriggerSurvivesAGatedCall )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 1 ) );

	TestableEffect plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	const unsigned int triggerIndex = plugin.ParamIndex( "Trigger" );
	CHECK( triggerIndex != TestableEffect::NO_PARAM );

	plugin.SetTime( 1.0 );
	ProcessOpenGLStruct frame = host.Frame();
	plugin.ProcessOpenGL( &frame );

	// Press the button, then let a gated call land on it. Consuming triggers on
	// a call that never read them would swallow the press and the button would
	// silently do nothing.
	plugin.SetFloatParameter( triggerIndex, 1.0f );
	plugin.ProcessOpenGL( &frame );
	CHECK( plugin.GetFloatParameter( triggerIndex ) > 0.5f );

	// Now let time move: the press is read, and only then consumed.
	plugin.SetTime( 2.0 );
	plugin.ProcessOpenGL( &frame );
	CHECK( plugin.GetFloatParameter( triggerIndex ) < 0.5f );

	plugin.DeInitGL();
	host.Teardown();
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

TEST( ParametersMapToTheRightFields )
{
	TestableEffect plugin;

	auto set = [ &plugin ]( const char* name, float value ) {
		const unsigned int index = plugin.ParamIndex( name );
		CHECK( index != TestableEffect::NO_PARAM );
		plugin.SetFloatParameter( index, value );
	};

	set( "Mosh Amount", 0.75f );
	set( "Motion Gain", 2.5f );
	set( "Motion Threshold", 0.6f );
	set( "Softness", 0.2f );
	set( "Pixel Snap", 0.1f );
	set( "Decay", 0.3f );
	set( "Corruption", 0.45f );
	set( "Chroma Drift", 0.55f );
	set( "Motion Lag", 5.0f );
	set( "Block Repeat", 0.35f );
	set( "Quantise", 0.65f );
	set( "Mix", 0.8f );
	set( "Invert Motion", 1.0f );
	// Options are addressed by position: 3 is the fourth entry, "32".
	set( "Block Size", 3.0f );
	// Third entry of Auto Mode is On Beat.
	set( "Auto Mode", 2.0f );
	set( "Quality", 0.0f );

	const MoshParams params = plugin.ReadParams();

	CHECK_NEAR( params.moshAmount, 0.75, 1e-5 );
	CHECK_NEAR( params.motionGain, 2.5, 1e-5 );
	CHECK_NEAR( params.motionThreshold, 0.6, 1e-5 );
	CHECK_NEAR( params.softness, 0.2, 1e-5 );
	CHECK_NEAR( params.pelSnap, 0.1, 1e-5 );
	CHECK_NEAR( params.decay, 0.3, 1e-5 );
	CHECK_NEAR( params.corruption, 0.45, 1e-5 );
	CHECK_NEAR( params.chromaDrift, 0.55, 1e-5 );
	CHECK( params.motionLag == 5 );
	CHECK_NEAR( params.blockRepeat, 0.35, 1e-5 );
	CHECK_NEAR( params.motionQuantise, 0.65, 1e-5 );
	CHECK_NEAR( params.mix, 0.8, 1e-5 );
	CHECK( params.invertDirection );

	// The distinction that bites: an option's position is not its value. Block
	// Size 3 selects the entry labelled "32", and 32 is what the search must get.
	CHECK( params.blockSize == 32 );
	CHECK( params.autoMode == AutoMode::OnBeat );
	CHECK( params.quality == Quality::Low );
}

// ---------------------------------------------------------------------------
// Audio
//
// The audio level is the one parameter that never passes through ReadParams —
// it is read off the SDK's analyser at render time — so ParametersMapToTheRight-
// Fields could not see that it was returning zero for every band, forever.
//
// ffglqs::Audio computes each bin as fft[i] * fft[i] * gain, and gain starts at
// 0. Nothing in the SDK sets it; the plugin has to. Miss that line and the
// audio parameters are inert, with no symptom other than nothing happening.
// ---------------------------------------------------------------------------

/// Delivers an FFT frame the way the host does: element values on the FFT
/// parameter, then the base's own refresh.
static void FeedAudio( TestableEffect& plugin, float magnitude )
{
	CHECK( plugin.audioParam != nullptr );
	for( size_t bin = 0; bin < plugin.audioParam->fftData.size(); ++bin )
		plugin.SetParamElementValue( plugin.audioParam->index, (unsigned int)bin, magnitude );

	// Smoothed, so one frame only moves the value part of the way there.
	for( int frame = 0; frame < 32; ++frame )
		plugin.UpdateAudioAndTime();
}

TEST( EveryAudioBandRespondsToHostFFTData )
{
	static const char* const BANDS[] = { "Volume", "Bass" };

	for( int band = 0; band < 2; ++band )
	{
		TestableEffect plugin;
		plugin.SetFloatParameter( plugin.ParamIndex( "Audio Band" ), (float)band );

		// Silence reads as silence.
		FeedAudio( plugin, 0.0f );
		CHECK_NEAR( plugin.AudioLevel(), 0.0, 1e-4 );

		// A full-scale spectrum has to move it. This is the assertion that fails
		// when the analyser's gain is left at zero — and it fails for both bands
		// at once, which is the signature of a gain problem rather than a
		// band-selection one.
		FeedAudio( plugin, 1.0f );
		const float loud = plugin.AudioLevel();
		if( loud <= 0.0f )
			std::fprintf( stderr, "  band %s read %f\n", BANDS[ band ], loud );
		CHECK( loud > 0.0f );
	}
}

TEST( AudioBandSelectionPicksDifferentPartsOfTheSpectrum )
{
	// Energy only in the bottom quarter. Bass averages over the bottom third and
	// Volume over the whole buffer, so with the signal down low Bass must read
	// higher — which is what distinguishes a working band selector from one that
	// happens to return the same number whatever is selected.
	auto levelFor = []( int band ) {
		TestableEffect plugin;
		plugin.SetFloatParameter( plugin.ParamIndex( "Audio Band" ), (float)band );

		const size_t bins = plugin.audioParam->fftData.size();
		for( size_t bin = 0; bin < bins; ++bin )
			plugin.SetParamElementValue( plugin.audioParam->index, (unsigned int)bin,
			                             bin < bins / 4 ? 1.0f : 0.0f );
		for( int frame = 0; frame < 32; ++frame )
			plugin.UpdateAudioAndTime();
		return plugin.AudioLevel();
	};

	const float bass   = levelFor( 1 );
	const float volume = levelFor( 0 );
	CHECK( bass > 0.0f );
	CHECK( volume > 0.0f );
	CHECK( bass > volume );
}

TEST( StyleAppliesAPresetAndRevertsWhenTouched )
{
	TestableEffect plugin;

	const unsigned int styleIndex = plugin.ParamIndex( "Style" );
	CHECK( styleIndex != TestableEffect::NO_PARAM );

	// Corrupt is the style that turns on every damage control at once.
	plugin.SetFloatParameter( styleIndex, static_cast< float >( Style::Corrupt ) );

	MoshParams params = plugin.ReadParams();
	CHECK( params.corruption > 0.0f );
	CHECK( params.blockRepeat > 0.0f );
	CHECK( params.motionQuantise > 0.0f );
	CHECK( params.motionLag > 0 );
	CHECK_NEAR( plugin.GetFloatParameter( styleIndex ), static_cast< float >( Style::Corrupt ), 1e-5 );

	// Liquid is the opposite end, and must clear what Corrupt set rather than
	// layering on top of it.
	plugin.SetFloatParameter( styleIndex, static_cast< float >( Style::Liquid ) );
	params = plugin.ReadParams();
	CHECK_NEAR( params.corruption, 0.0, 1e-5 );
	CHECK_NEAR( params.softness, 1.0, 1e-5 );
	CHECK( params.blockSize == 8 );

	// Touching anything else means the settings no longer match the preset, and
	// the dropdown must stop claiming they do.
	plugin.SetFloatParameter( plugin.ParamIndex( "Softness" ), 0.5f );
	CHECK_NEAR( plugin.GetFloatParameter( styleIndex ), static_cast< float >( Style::Custom ), 1e-5 );
}

// ---------------------------------------------------------------------------
// Host instantiation
// ---------------------------------------------------------------------------

/// Replays what FFGL's instantiateGL does before it will hand a plugin to the
/// host: walk every advertised parameter index and write its declared default.
/// A single FF_FAIL there and the instance is destroyed and the plugin never
/// loads, with nothing logged anywhere.
template< typename PluginType >
bool SurvivesHostDefaultInitialisation( PluginType& plugin, unsigned int& failedAt )
{
	for( unsigned int index = 0; index < plugin.GetNumParams(); ++index )
	{
		const unsigned int type = plugin.GetParamType( index );
		if( type == FF_TYPE_TEXT || type == FF_TYPE_FILE )
			continue;

		const FFMixed declared = plugin.GetParamDefault( index );
		float value = 0.0f;
		std::memcpy( &value, &declared.UIntValue, sizeof( float ) );

		if( plugin.SetFloatParameter( index, value ) == FF_FAIL )
		{
			failedAt = index;
			return false;
		}
	}
	return true;
}

TEST( BothPluginsSurviveHostInstantiation )
{
	// This exists because of a real defect: renaming the mixer's inherited
	// parameter with SetParamInfo appended a phantom record instead of updating
	// index 0, since that call unconditionally push_backs. GetNumParams then
	// reported one more parameter than existed, the default-init walk hit the
	// phantom, and the mixer could not be instantiated by any host at all.
	{
		TestableEffect effect;
		unsigned int   failedAt = 0;
		CHECK( SurvivesHostDefaultInitialisation( effect, failedAt ) );
		// Every advertised parameter must have something behind it.
		CHECK( effect.GetNumParams() == effect.ParamCount() );
	}
	{
		TestableMixer mixer;
		unsigned int  failedAt = 0;
		CHECK( SurvivesHostDefaultInitialisation( mixer, failedAt ) );
		CHECK( mixer.GetNumParams() == mixer.ParamCount() );
	}
}

TEST( StyleSurvivesUnrelatedParameterWrites )
{
	TestableEffect plugin;

	const unsigned int styleIndex = plugin.ParamIndex( "Style" );
	CHECK( styleIndex != TestableEffect::NO_PARAM );

	plugin.SetFloatParameter( styleIndex, static_cast< float >( Style::Bloom ) );

	// None of these is something a style sets, so none of them means the
	// operator has departed from the preset. Anything driven from MIDI is
	// written every frame, so a style that reset on any write could never stay.
	const char* const unrelated[] = { "Trigger", "Reset", "Mix", "Quality",
	                                  "Auto Mode", "Cut Sensitivity", "Audio Amount" };
	for( const char* name : unrelated )
	{
		const unsigned int index = plugin.ParamIndex( name );
		CHECK( index != TestableEffect::NO_PARAM );
		plugin.SetFloatParameter( index, 1.0f );
	}
	CHECK_NEAR( plugin.GetFloatParameter( styleIndex ), static_cast< float >( Style::Bloom ), 1e-5 );

	// A host echoing back exactly what the style set is not a change either.
	const unsigned int smoothingIndex = plugin.ParamIndex( "Motion Smoothing" );
	plugin.SetFloatParameter( smoothingIndex, plugin.GetFloatParameter( smoothingIndex ) );
	CHECK_NEAR( plugin.GetFloatParameter( styleIndex ), static_cast< float >( Style::Bloom ), 1e-5 );

	// Actually moving one of the style's own parameters does depart from it.
	plugin.SetFloatParameter( smoothingIndex, 0.25f );
	CHECK_NEAR( plugin.GetFloatParameter( styleIndex ), static_cast< float >( Style::Custom ), 1e-5 );
}

// ---------------------------------------------------------------------------
// The mixer
// ---------------------------------------------------------------------------

TEST( MixerTakesMotionFromTheSelectedInput )
{
	// Input 0 is the destination (the layers below), input 1 the source (this
	// layer's clip). Feeding motion to one and stillness to the other makes the
	// selection observable: the estimated field is either moving or it is not.
	const auto meanMotion = []( int motionSource ) {
		Host host;
		if( !host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 2 ) )
			return -1.0f;

		TestableMixer plugin;
		const FFGLViewportStruct viewport = host.Viewport();
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
			return -1.0f;

		plugin.SetFloatParameter( plugin.ParamIndex( "Motion Source" ),
		                          static_cast< float >( motionSource ) );

		for( int frame = 0; frame < 8; ++frame )
		{
			// Input 1 moves, input 0 never does.
			host.Fill( 0, 0.0f, 0.0f );
			host.Fill( 1, static_cast< float >( frame * 3 ), 0.0f );
			plugin.SetTime( static_cast< double >( frame ) );
			ProcessOpenGLStruct pGL = host.Frame();
			plugin.ProcessOpenGL( &pGL );
		}

		const std::vector< float > flow = ReadTarget( plugin.pipeline.GetFlowField() );
		float total = 0.0f;
		for( size_t index = 0; index < flow.size(); index += 4 )
			total += std::fabs( flow[ index ] );
		const float mean = flow.empty() ? 0.0f : total / ( flow.size() / 4 );

		plugin.DeInitGL();
		host.Teardown();
		return mean;
	};

	// "This Layer" is input 1, the one that is moving.
	const float fromThisLayer = meanMotion( 0 );
	// "Layer Below" is input 0, which is static.
	const float fromBelow = meanMotion( 1 );

	CHECK( fromThisLayer > 0.0f );
	CHECK( fromBelow >= 0.0f );
	CHECK( fromThisLayer > fromBelow * 4.0f );
}

TEST( MixerHandlesInputsOfDifferentSizes )
{
	Host host;
	CHECK( host.Setup( FRAME_WIDTH, FRAME_HEIGHT, 2 ) );
	// Nothing guarantees a layer and the ones below it share a resolution.
	// Buffers follow the pixel source and the motion input is resampled into it.
	CHECK( host.ResizeInput( 1, 96, 64 ) );

	TestableMixer plugin;
	const FFGLViewportStruct viewport = host.Viewport();
	CHECK( plugin.InitGL( &viewport ) == FF_SUCCESS );

	for( int frame = 0; frame < 6; ++frame )
	{
		host.Fill( 0, static_cast< float >( frame * 2 ), 0.0f );
		host.Fill( 1, static_cast< float >( frame * 3 ), 0.0f );
		plugin.SetTime( static_cast< double >( frame ) );
		ProcessOpenGLStruct pGL = host.Frame();
		CHECK( plugin.ProcessOpenGL( &pGL ) == FF_SUCCESS );
	}

	// Geometry comes from the pixel source, which is input 0.
	CHECK( plugin.pipeline.GetWidth() == FRAME_WIDTH );
	CHECK( plugin.pipeline.GetHeight() == FRAME_HEIGHT );
	CHECK( AllFinite( ReadTarget( plugin.pipeline.GetAccumulation() ) ) );

	plugin.DeInitGL();
	host.Teardown();
}

}  // namespace datamosh::test
