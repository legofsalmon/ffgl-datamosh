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
#include <cmath>
#include <memory>
#include <string>
#include <vector>

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
	FFResult SetFloatParameter( unsigned int index, float value ) override;

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

	/// Adds a parameter and files it under a collapsible region.
	///
	/// Consecutive parameters sharing a group name collapse together in
	/// Resolume 7.3 and later. Without this the panel is one flat column of
	/// twenty-odd sliders, which is unusable to reach into mid-set.
	template< typename ParamType >
	void AddGrouped( const char* group, std::shared_ptr< ParamType > param )
	{
		const unsigned int index = static_cast< unsigned int >( this->params.size() );
		this->AddParam( param );
		this->SetParamGroup( index, group );
	}

	/// Index of a parameter by name, or NO_PARAM.
	unsigned int ParamIndex( const char* name ) const;
	/// Sets a parameter and tells the host, so its slider follows.
	void PushParam( const char* name, float value );
	/// Sets an option parameter by the value shown in the dropdown rather than
	/// by its position in the list.
	void PushOption( const char* name, float realValue );
	/// Writes a whole coherent set of parameters.
	void ApplyStyle( int style );
	/// Records the style-managed values as the style left them, so a later write
	/// can be told apart from an echo of what was just set.
	void SnapshotStyleValues();
	/// Position of `index` within styleManagedIndices, or -1.
	int  StyleManagedSlot( unsigned int index ) const;
	/// The selected band's level, read off the SDK's analyser. Its own method
	/// rather than inline in ProcessOpenGL so a test can reach it: this value
	/// never passes through ReadParams, which is how it went unnoticed that the
	/// whole audio path was returning zero.
	float AudioLevel();

	static constexpr unsigned int NO_PARAM = 0xFFFFFFFFu;

	MoshPipeline pipeline;

	std::shared_ptr< ffglqs::ParamFFT > audioParam;

	unsigned int styleParamIndex = NO_PARAM;
	/// Guards the reentry caused by a style writing its own parameters back
	/// through the host's setter.
	bool         applyingStyle   = false;
	/// The parameters a style actually writes. Anything outside this set cannot
	/// invalidate a style, so pressing Trigger does not silently change it.
	std::vector< unsigned int > styleManagedIndices;
	/// Their values as of the last ApplyStyle, in the same order.
	std::vector< float >        styleSnapshot;

	double lastHostTime  = 0.0;
	bool   hostTimeValid = false;
	int    frameCounter  = 0;
	/// Wall-clock time banked by calls the frame gate skipped, so a burst still
	/// runs for the number of seconds it was set to.
	float  pendingDeltaTime = 0.0f;
	/// The delta actually handed to the last advance. Kept so the banking is
	/// observable from a test and from a log, rather than only inferable.
	float  lastAppliedDeltaTime = 0.0f;
};

// ---------------------------------------------------------------------------

template< typename HostBase >
DatamoshPlugin< HostBase >::DatamoshPlugin() :
	HostBase()
{
}

/// The preset styles offered by the Style dropdown, in dropdown order.
///
/// Reaching for twelve sliders is not something anyone does while a set is
/// running. Each of these is a coherent starting point that can then be nudged.
enum class Style : int
{
	Custom = 0,
	Melt,
	Bloom,
	Drag,
	Liquid,
	Corrupt,
};

template< typename HostBase >
void DatamoshPlugin< HostBase >::DeclareCommonParams()
{
	using ffglqs::ParamBool;
	using ffglqs::ParamFFT;
	using ffglqs::ParamOption;
	using ffglqs::ParamRange;
	using ffglqs::ParamTrigger;
	using Range = ffglqs::ParamRange::Range;

	// --- when pixels stop refreshing ---
	styleParamIndex = static_cast< unsigned int >( this->params.size() );
	AddGrouped( "Mosh", ParamOption::Create( "Style",
	                                         { { "Custom", 0.0f },
	                                           { "Melt", 1.0f },
	                                           { "Bloom", 2.0f },
	                                           { "Drag", 3.0f },
	                                           { "Liquid", 4.0f },
	                                           { "Corrupt", 5.0f } },
	                                         0 ) );
	AddGrouped( "Mosh", ParamRange::Create( "Mosh Amount", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Mosh", ParamTrigger::Create( "Trigger" ) );
	AddGrouped( "Mosh", ParamTrigger::Create( "Reset" ) );
	AddGrouped( "Mosh", ParamOption::Create( "Auto Mode",
	                                         { { "Manual", 0.0f }, { "On Cut", 1.0f }, { "On Beat", 2.0f } },
	                                         0 ) );
	AddGrouped( "Mosh", ParamRange::Create( "Cut Sensitivity", 0.5f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Mosh", ParamRange::Create( "Burst Length", 1.0f, Range( 0.05f, 8.0f ) ) );
	AddGrouped( "Mosh", ParamOption::Create( "Beat Divisor",
	                                         { { "1", 1.0f }, { "2", 2.0f }, { "4", 4.0f }, { "8", 8.0f }, { "16", 16.0f } },
	                                         2 ) );

	// --- how the motion behaves ---
	AddGrouped( "Motion", ParamRange::Create( "Motion Gain", 1.0f, Range( 0.0f, 4.0f ) ) );
	AddGrouped( "Motion", ParamRange::Create( "Freeze", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Motion", ParamRange::Create( "Motion Smoothing", 0.3f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Motion", ParamRange::Create( "Motion Threshold", 0.15f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Motion", ParamOption::Create( "Block Size",
	                                           { { "4", 4.0f }, { "8", 8.0f }, { "16", 16.0f }, { "32", 32.0f } },
	                                           2 ) );
	AddGrouped( "Motion", ParamRange::Create( "Softness", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Motion", ParamRange::Create( "Pel Snap", 1.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Motion", ParamBool::Create( "Invert Motion", false ) );

	// --- getting it wrong on purpose ---
	AddGrouped( "Damage", ParamRange::CreateInteger( "Motion Lag", 0, Range( 0.0f, 15.0f ) ) );
	AddGrouped( "Damage", ParamRange::Create( "Block Repeat", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Damage", ParamRange::Create( "Quantise", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Damage", ParamRange::Create( "Corruption", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Damage", ParamRange::Create( "Chroma Drift", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Damage", ParamRange::Create( "Decay", 0.0f, Range( 0.0f, 1.0f ) ) );

	// --- sync ---
	audioParam = ParamFFT::Create( "Audio" );
	AddGrouped( "Sync", audioParam );
	// ffglqs::Audio starts with gain 0 and every bin is fft[i] * fft[i] * gain,
	// so without this line every band reads exactly zero forever and the audio
	// parameters are inert with no symptom other than nothing happening. The
	// argument is decibels — 0 dB is unity, not silence.
	this->audioParams[ audioParam ].SetGain( 0.0f );
	AddGrouped( "Sync", ParamRange::Create( "Audio Amount", 0.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Sync", ParamOption::Create( "Audio Band",
	                                         { { "Volume", 0.0f }, { "Bass", 1.0f }, { "Mid", 2.0f }, { "High", 3.0f } },
	                                         1 ) );

	// --- output ---
	AddGrouped( "Output", ParamRange::Create( "Mix", 1.0f, Range( 0.0f, 1.0f ) ) );
	AddGrouped( "Output", ParamOption::Create( "Quality",
	                                           { { "Low", 0.0f }, { "Medium", 1.0f }, { "High", 2.0f }, { "Ultra", 3.0f } },
	                                           2 ) );

	// Exactly the parameters ApplyStyle writes. Nothing else can invalidate a
	// style, so Trigger, Mix, Quality and the rest leave the dropdown alone.
	static const char* const MANAGED[] = {
		"Mosh Amount", "Motion Gain", "Freeze", "Motion Smoothing", "Motion Threshold",
		"Softness", "Pel Snap", "Motion Lag", "Block Repeat", "Quantise",
		"Corruption", "Chroma Drift", "Decay", "Block Size"
	};
	for( const char* name : MANAGED )
	{
		const unsigned int index = ParamIndex( name );
		if( index != NO_PARAM )
			styleManagedIndices.push_back( index );
	}
	SnapshotStyleValues();
}

template< typename HostBase >
void DatamoshPlugin< HostBase >::SnapshotStyleValues()
{
	styleSnapshot.clear();
	styleSnapshot.reserve( styleManagedIndices.size() );
	for( unsigned int index : styleManagedIndices )
		styleSnapshot.push_back( this->params[ index ]->GetValue() );
}

template< typename HostBase >
int DatamoshPlugin< HostBase >::StyleManagedSlot( unsigned int index ) const
{
	for( size_t slot = 0; slot < styleManagedIndices.size(); ++slot )
	{
		if( styleManagedIndices[ slot ] == index )
			return static_cast< int >( slot );
	}
	return -1;
}

template< typename HostBase >
float DatamoshPlugin< HostBase >::AudioLevel()
{
	if( !audioParam )
		return 0.0f;

	ffglqs::Audio& audio = this->audioParams[ audioParam ];
	switch( OptionValue( "Audio Band" ) )
	{
	case 1:  return audio.GetBass();
	case 2:  return audio.GetMed();
	case 3:  return audio.GetHigh();
	default: return audio.GetVolume();
	}
}

template< typename HostBase >
unsigned int DatamoshPlugin< HostBase >::ParamIndex( const char* name ) const
{
	for( size_t index = 0; index < this->params.size(); ++index )
	{
		if( this->params[ index ]->GetName() == name )
			return static_cast< unsigned int >( index );
	}
	return NO_PARAM;
}

template< typename HostBase >
void DatamoshPlugin< HostBase >::PushParam( const char* name, float value )
{
	const unsigned int index = ParamIndex( name );
	if( index == NO_PARAM )
		return;
	this->params[ index ]->SetValue( value );
	// Without the event the value changes but the slider does not move, so the
	// panel shows something other than what is rendering.
	this->RaiseParamEvent( index, FF_EVENT_FLAG_VALUE );
}

template< typename HostBase >
void DatamoshPlugin< HostBase >::PushOption( const char* name, float realValue )
{
	auto param = this->GetParamOption( name );
	if( !param )
		return;
	// Option parameters are addressed by position, not by the number shown, so
	// "block size 16" has to be looked up rather than assigned.
	for( size_t position = 0; position < param->options.size(); ++position )
	{
		if( param->options[ position ].value == realValue )
		{
			PushParam( name, static_cast< float >( position ) );
			return;
		}
	}
}

template< typename HostBase >
void DatamoshPlugin< HostBase >::ApplyStyle( int style )
{
	if( static_cast< Style >( style ) == Style::Custom )
		return;

	// Every PushParam below comes back through SetFloatParameter, which would
	// otherwise read as the operator touching a slider and reset Style to Custom.
	applyingStyle = true;

	// Shared starting point, so each style only states what makes it itself.
	PushParam( "Mosh Amount", 1.0f );
	PushParam( "Motion Gain", 1.0f );
	PushParam( "Freeze", 0.0f );
	PushParam( "Motion Smoothing", 0.3f );
	PushParam( "Motion Threshold", 0.0f );
	PushParam( "Softness", 0.0f );
	PushParam( "Pel Snap", 1.0f );
	PushParam( "Motion Lag", 0.0f );
	PushParam( "Block Repeat", 0.0f );
	PushParam( "Quantise", 0.0f );
	PushParam( "Corruption", 0.0f );
	PushParam( "Chroma Drift", 0.0f );
	PushParam( "Decay", 0.0f );
	PushOption( "Block Size", 16.0f );

	switch( static_cast< Style >( style ) )
	{
	case Style::Melt:
		// Pixels carried along by their own motion, refreshing never.
		PushParam( "Motion Gain", 1.2f );
		PushParam( "Motion Smoothing", 0.4f );
		PushParam( "Softness", 0.3f );
		PushParam( "Motion Lag", 2.0f );
		break;

	case Style::Bloom:
		// Vector field held, so motion piles up on itself and explodes.
		PushParam( "Freeze", 1.0f );
		PushParam( "Motion Gain", 1.5f );
		PushParam( "Motion Smoothing", 0.6f );
		PushParam( "Softness", 0.2f );
		PushParam( "Decay", 0.1f );
		break;

	case Style::Drag:
		// Only what is moving fast smears, and it smears hard.
		PushParam( "Motion Threshold", 0.5f );
		PushParam( "Motion Gain", 2.0f );
		PushParam( "Motion Smoothing", 0.2f );
		PushOption( "Block Size", 32.0f );
		break;

	case Style::Liquid:
		// Per-pixel flow with no snapping: the smooth end of the range.
		PushParam( "Softness", 1.0f );
		PushParam( "Motion Smoothing", 0.5f );
		PushParam( "Pel Snap", 0.0f );
		PushOption( "Block Size", 8.0f );
		break;

	case Style::Corrupt:
		// Everything that makes the vectors wrong, at once.
		PushParam( "Motion Threshold", 0.1f );
		PushParam( "Corruption", 0.4f );
		PushParam( "Block Repeat", 0.3f );
		PushParam( "Quantise", 0.7f );
		PushParam( "Chroma Drift", 0.4f );
		PushParam( "Motion Lag", 4.0f );
		break;

	case Style::Custom:
		break;
	}

	applyingStyle = false;
	SnapshotStyleValues();
}

template< typename HostBase >
FFResult DatamoshPlugin< HostBase >::SetFloatParameter( unsigned int index, float value )
{
	const FFResult result = HostBase::SetFloatParameter( index, value );

	if( applyingStyle || styleParamIndex == NO_PARAM )
		return result;

	if( index == styleParamIndex )
	{
		ApplyStyle( static_cast< int >( value ) );
		return result;
	}

	// Only the parameters a style actually writes can invalidate it. Without
	// this, pressing Trigger or nudging Mix would silently flip the dropdown to
	// Custom, and anything driven from MIDI is written every frame, so no style
	// could ever stay selected.
	const int slot = StyleManagedSlot( index );
	if( slot < 0 || slot >= static_cast< int >( styleSnapshot.size() ) )
		return result;

	// Applying a style raises a value event per parameter, and a host may write
	// that value straight back after applyingStyle has been cleared. An echo of
	// what the style itself set is not the operator changing anything.
	if( std::fabs( styleSnapshot[ slot ] - value ) < 1e-4f )
		return result;

	// The settings on screen no longer match the preset, and the dropdown must
	// stop claiming they do. This also fires while a host restores a saved
	// composition, so a comp saved on a style reloads as Custom carrying the
	// right values rather than as the style it came from.
	this->params[ styleParamIndex ]->SetValue( static_cast< float >( Style::Custom ) );
	this->RaiseParamEvent( styleParamIndex, FF_EVENT_FLAG_VALUE );

	return result;
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

	params.decay          = ParamValue( "Decay" );
	params.corruption     = ParamValue( "Corruption" );
	params.chromaDrift    = ParamValue( "Chroma Drift" );
	params.mix            = ParamValue( "Mix" );
	params.motionLag      = static_cast< int >( ParamValue( "Motion Lag" ) + 0.5f );
	params.blockRepeat    = ParamValue( "Block Repeat" );
	params.motionQuantise = ParamValue( "Quantise" );

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

	params.audioLevel = AudioLevel();

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
		params.deltaTime     = std::min( 0.25f, pendingDeltaTime );
		lastAppliedDeltaTime = params.deltaTime;
		advanced             = pipeline.Advance( inputs, params );
		pendingDeltaTime     = 0.0f;
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
