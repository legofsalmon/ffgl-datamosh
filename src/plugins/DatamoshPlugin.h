#pragma once

#include <MoshParams.h>
#include <MoshPipeline.h>

#include <ffgl/FFGLLog.h>
#include <ffglquickstart/FFGLEffect.h>
#include <ffglquickstart/FFGLMixer.h>
#include <ffglquickstart/FFGLParamOption.h>
#include <ffglquickstart/FFGLParamRange.h>
#include <ffglquickstart/FFGLParamTrigger.h>
#include <ffglquickstart/FFGLParamFFT.h>
#include <ffglquickstart/FFGLParamBool.h>

#include <algorithm>
#include <memory>
#include <string>

namespace datamosh {

/// Shared behaviour for both plugin binaries.
///
/// Templated on the quickstart base because Resolume surfaces one-input and
/// two-input plugins through different classes, but everything that makes this
/// a datamosh — parameters, control mapping, the pipeline — is identical.
/// FFGL forces the two builds to be separate binaries anyway (FFGL.cpp
/// dispatches through a single global plugin record), so sharing here is the
/// only sharing available.
template< typename HostBase >
class DatamoshPlugin : public HostBase
{
public:
	DatamoshPlugin();
	~DatamoshPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* viewPort ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;
	FFResult SetTime( double time ) override;

protected:
	/// Fills in the FrameInputs for this plugin type. The effect points both
	/// pixel and motion at its single input; the mixer splits them.
	virtual bool GatherInputs( ProcessOpenGLStruct* pGL, FrameInputs& inputs ) = 0;

	/// Parameters shared by both builds. Subclasses add their own first or after.
	void DeclareCommonParams();

	/// Reads every parameter into the plain struct the pipeline consumes.
	MoshParams ReadParams() const;

	/// Hook for a subclass to fold in parameters only it has.
	virtual void AdjustParams( MoshParams& params ) const { ( void )params; }

	float ParamValue( const char* name ) const;
	int   OptionValue( const char* name ) const;

	MoshPipeline pipeline;

	std::shared_ptr< ffglqs::ParamFFT > audioParam;

	double lastHostTime  = 0.0;
	bool   hostTimeValid = false;
	int    frameCounter  = 0;
	/// Wall-clock time banked by calls the frame gate skipped, so a burst still
	/// runs for the number of seconds it was set to.
	float  pendingDeltaTime = 0.0f;
};

// ---------------------------------------------------------------------------

template< typename HostBase >
DatamoshPlugin< HostBase >::DatamoshPlugin() :
	HostBase()
{
}

template< typename HostBase >
void DatamoshPlugin< HostBase >::DeclareCommonParams()
{
	using ffglqs::Param;
	using ffglqs::ParamBool;
	using ffglqs::ParamFFT;
	using ffglqs::ParamOption;
	using ffglqs::ParamRange;
	using ffglqs::ParamTrigger;
	using Range  = ffglqs::ParamRange::Range;
	using Option = ffglqs::ParamOption::Option;

	// --- when pixels stop refreshing ---
	this->AddParam( ParamRange::Create( "Mosh Amount", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamTrigger::Create( "Trigger" ) );
	this->AddParam( ParamTrigger::Create( "Reset" ) );
	this->AddParam( ParamOption::Create( "Auto Mode",
	                                     { { "Manual", 0.0f }, { "On Cut", 1.0f }, { "On Beat", 2.0f } },
	                                     0 ) );
	this->AddParam( ParamRange::Create( "Cut Sensitivity", 0.5f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Burst Length", 1.0f, Range( 0.05f, 8.0f ) ) );
	this->AddParam( ParamOption::Create( "Beat Divisor",
	                                     { { "1", 1.0f }, { "2", 2.0f }, { "4", 4.0f }, { "8", 8.0f }, { "16", 16.0f } },
	                                     2 ) );

	// --- how the motion behaves ---
	this->AddParam( ParamRange::Create( "Motion Gain", 1.0f, Range( 0.0f, 4.0f ) ) );
	this->AddParam( ParamRange::Create( "Freeze", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Motion Smoothing", 0.3f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Motion Threshold", 0.15f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamOption::Create( "Block Size",
	                                     { { "4", 4.0f }, { "8", 8.0f }, { "16", 16.0f }, { "32", 32.0f } },
	                                     2 ) );
	this->AddParam( ParamRange::Create( "Softness", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Pel Snap", 1.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamBool::Create( "Invert Motion", false ) );

	// --- damage ---
	this->AddParam( ParamRange::Create( "Decay", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Corruption", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Chroma Drift", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamRange::Create( "Mix", 1.0f, Range( 0.0f, 1.0f ) ) );

	// --- sync ---
	audioParam = ParamFFT::Create( "Audio" );
	this->AddParam( audioParam );
	this->AddParam( ParamRange::Create( "Audio Amount", 0.0f, Range( 0.0f, 1.0f ) ) );
	this->AddParam( ParamOption::Create( "Audio Band",
	                                     { { "Volume", 0.0f }, { "Bass", 1.0f }, { "Mid", 2.0f }, { "High", 3.0f } },
	                                     1 ) );

	// --- cost ---
	this->AddParam( ParamOption::Create( "Quality",
	                                     { { "Low", 0.0f }, { "Medium", 1.0f }, { "High", 2.0f }, { "Ultra", 3.0f } },
	                                     2 ) );
}

template< typename HostBase >
float DatamoshPlugin< HostBase >::ParamValue( const char* name ) const
{
	// GetParam is non-const in the SDK but does not mutate; this keeps the
	// reading side honest about its intent.
	auto param = const_cast< DatamoshPlugin* >( this )->GetParam( name );
	return param ? param->GetValue() : 0.0f;
}

template< typename HostBase >
int DatamoshPlugin< HostBase >::OptionValue( const char* name ) const
{
	auto param = const_cast< DatamoshPlugin* >( this )->GetParamOption( name );
	// GetRealValue is the option's own value, not its index, so the numbers in
	// the dropdown ("16" for block size) are the numbers used.
	return param ? static_cast< int >( param->GetRealValue() ) : 0;
}

template< typename HostBase >
MoshParams DatamoshPlugin< HostBase >::ReadParams() const
{
	MoshParams params;

	params.moshAmount  = ParamValue( "Mosh Amount" );
	params.trigger     = ParamValue( "Trigger" ) > 0.5f;
	params.reset       = ParamValue( "Reset" ) > 0.5f;
	params.autoMode    = static_cast< AutoMode >( OptionValue( "Auto Mode" ) );
	params.sensitivity = ParamValue( "Cut Sensitivity" );
	params.duration    = ParamValue( "Burst Length" );
	params.beatDivisor = std::max( 1, OptionValue( "Beat Divisor" ) );

	params.motionGain      = ParamValue( "Motion Gain" );
	params.motionFreeze    = ParamValue( "Freeze" );
	params.motionSmoothing = ParamValue( "Motion Smoothing" );
	params.motionThreshold = ParamValue( "Motion Threshold" );
	params.blockSize       = std::max( 2, OptionValue( "Block Size" ) );
	params.softness        = ParamValue( "Softness" );
	params.pelSnap         = ParamValue( "Pel Snap" );
	params.invertDirection = ParamValue( "Invert Motion" ) > 0.5f;

	params.decay       = ParamValue( "Decay" );
	params.corruption  = ParamValue( "Corruption" );
	params.chromaDrift = ParamValue( "Chroma Drift" );
	params.mix         = ParamValue( "Mix" );

	params.audioAmount = ParamValue( "Audio Amount" );
	params.quality     = static_cast< Quality >( OptionValue( "Quality" ) );

	params.bpm      = this->bpm;
	params.barPhase = this->barPhase;

	AdjustParams( params );
	return params;
}

template< typename HostBase >
FFResult DatamoshPlugin< HostBase >::InitGL( const FFGLViewportStruct* viewPort )
{
	// Deliberately not calling HostBase::InitGL. The quickstart base synthesises
	// and compiles a fragment shader from the parameter names, which this plugin
	// never draws with — it runs its own multi-pass graph. Skipping it also frees
	// the parameters from having to be valid GLSL identifiers, so they can be
	// named for the operator reading them ("Motion Gain", not "Motion_Gain").
	if( !pipeline.Initialise() )
	{
		FFGLLog::LogToHost( "datamosh: pipeline failed to initialise" );
		return FF_FAIL;
	}

	return CFFGLPlugin::InitGL( viewPort );
}

template< typename HostBase >
FFResult DatamoshPlugin< HostBase >::DeInitGL()
{
	pipeline.Release();
	return FF_SUCCESS;
}

template< typename HostBase >
FFResult DatamoshPlugin< HostBase >::SetTime( double time )
{
	hostTimeValid = true;
	return HostBase::SetTime( time );
}

template< typename HostBase >
FFResult DatamoshPlugin< HostBase >::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL == nullptr )
		return FF_FAIL;

	// Refreshes the FFT buffers and the wall-clock delta. Normally called by
	// the quickstart base's ProcessOpenGL, which we are replacing.
	this->UpdateAudioAndTime();

	FrameInputs inputs;
	if( !GatherInputs( pGL, inputs ) )
		return FF_FAIL;

	MoshParams params = ReadParams();

	// Wall-clock delta, clamped. An unclamped delta from a stalled frame would
	// expire a burst instantly and make the effect look like it dropped out.
	// It is banked rather than used directly because the gate below can skip a
	// call, and time that passed during a skipped call still passed.
	pendingDeltaTime += std::min( 0.1f, std::max( 1.0f / 240.0f, this->deltaTime ) );
	params.frame = frameCounter;

	if( audioParam )
	{
		const ffglqs::Audio& audio = this->audioParams[ audioParam ];
		switch( OptionValue( "Audio Band" ) )
		{
		case 1:  params.audioLevel = const_cast< ffglqs::Audio& >( audio ).GetBass(); break;
		case 2:  params.audioLevel = const_cast< ffglqs::Audio& >( audio ).GetMed(); break;
		case 3:  params.audioLevel = const_cast< ffglqs::Audio& >( audio ).GetHigh(); break;
		default: params.audioLevel = const_cast< ffglqs::Audio& >( audio ).GetVolume(); break;
		}
	}

	// Advance the simulation at most once per host frame. Resolume can call a
	// plugin more than once for the same moment in time, and a feedback effect
	// that advanced on every call would run at a different speed depending on
	// how the composition happens to be wired.
	//
	// The gate only engages once the host has actually delivered a time. If it
	// never does, hostTimeValid stays false and every call advances — a wrong
	// rate is recoverable, a permanently frozen effect is not.
	const bool timeMoved = !hostTimeValid || ( this->hostTime != lastHostTime );
	lastHostTime         = this->hostTime;

	bool advanced = true;
	if( timeMoved )
	{
		params.deltaTime = std::min( 0.25f, pendingDeltaTime );
		advanced         = pipeline.Advance( inputs, params );
		pendingDeltaTime = 0.0f;
		++frameCounter;

		// Only consume the one-shot buttons on a call that actually read them.
		// Consuming on a skipped call would swallow the press and the button
		// would appear to do nothing.
		this->consumeAllTrigger();
	}

	// HasHistory also covers the case where the very first call arrives with
	// time already settled: the gate would skip the advance, leaving nothing in
	// the accumulation buffer to composite.
	if( advanced && pipeline.HasHistory() )
		pipeline.Composite( pGL->HostFBO, params.mix );
	else
		// Never fail to a black frame: an effect that cannot run should cost the
		// operator the effect, not the output.
		pipeline.Passthrough( pGL->HostFBO, inputs );

	return FF_SUCCESS;
}

}  // namespace datamosh
