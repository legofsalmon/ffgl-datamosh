#include "DatamoshPlugin.h"

#include <ffgl/FFGLPluginInfo.h>
#include <ffglquickstart/FFGLPlugin.h>

namespace datamosh {

/// The two-input build, which Resolume shows in a layer's blend-mode dropdown.
///
/// This is where the effect gets closest to what datamoshing originally was.
/// Real datamoshing splices one shot's motion vectors onto another shot's
/// pixels; here the layer carrying the mixer supplies the motion and the layers
/// underneath supply the pixels, so a clip of moving water can be made to drag
/// a completely different image around.
class DatamoshMixer : public DatamoshPlugin< ffglqs::Mixer >
{
	/// ffglqs::Mixer registers its own parameter before any of ours, so this is
	/// always index 0.
	static constexpr unsigned int BLEND_PARAM_INDEX = 0;

public:
	DatamoshMixer()
	{
		using ffglqs::ParamOption;

		// ffglqs::Mixer's constructor has already registered a parameter called
		// "mixVal" at index 0, which nothing here would read — leaving a dead
		// slider sitting in the layer's Video panel right next to this plugin's
		// own Mix. Redeclaring index 0 gives it a name that says what it does and
		// a default of fully wet; AdjustParams below folds it into the wet/dry.
		if( auto blend = GetParam( "mixVal" ) )
			blend->SetValue( 1.0f );
		SetParamInfo( BLEND_PARAM_INDEX, "Blend", FF_TYPE_STANDARD, 1.0f );

		// FFGL's mixer convention: input 0 is the destination (what is already
		// on the layers below) and input 1 is the source (this layer's clip).
		// Taking motion from this layer and pixels from below is the useful
		// default — it makes the clip you drop the mixer onto the "motion clip".
		AddParam( ParamOption::Create( "Motion Source",
		                               { { "This Layer", 0.0f }, { "Layer Below", 1.0f } },
		                               0 ) );
		DeclareCommonParams();
	}

protected:
	void AdjustParams( MoshParams& params ) const override
	{
		params.mix *= ParamValue( "mixVal" );
	}

	bool GatherInputs( ProcessOpenGLStruct* pGL, FrameInputs& inputs ) override
	{
		if( pGL->numInputTextures < 2 )
			return false;
		if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
			return false;

		const FFGLTextureStruct& destination = *pGL->inputTextures[ 0 ];
		const FFGLTextureStruct& source      = *pGL->inputTextures[ 1 ];

		if( destination.Handle == 0 || source.Handle == 0 )
			return false;
		if( destination.Width == 0 || destination.Height == 0 )
			return false;

		const bool motionFromBelow = ( OptionValue( "Motion Source" ) == 1 );

		const FFGLTextureStruct& pixelTexture  = motionFromBelow ? source : destination;
		const FFGLTextureStruct& motionTexture = motionFromBelow ? destination : source;

		const FFGLTexCoords pixelCoords  = GetMaxGLTexCoords( pixelTexture );
		const FFGLTexCoords motionCoords = GetMaxGLTexCoords( motionTexture );

		inputs.pixelTexture  = pixelTexture.Handle;
		inputs.pixelMaxU     = pixelCoords.s;
		inputs.pixelMaxV     = pixelCoords.t;
		inputs.motionTexture = motionTexture.Handle;
		inputs.motionMaxU    = motionCoords.s;
		inputs.motionMaxV    = motionCoords.t;

		// Buffers follow the pixel source; the motion input is resampled into
		// that geometry, so the two inputs need not share a resolution.
		inputs.width  = static_cast< GLsizei >( pixelTexture.Width );
		inputs.height = static_cast< GLsizei >( pixelTexture.Height );
		return true;
	}
};

}  // namespace datamosh

static CFFGLPluginInfo PluginInfo(
	PluginFactory< datamosh::DatamoshMixer >,
	"DMMX",              // unique ID, maximum four characters
	"Datamosh Transplant",// name shown in Resolume's blend-mode list
	2,                   // FFGL API major
	1,                   // FFGL API minor
	0,                   // plugin major
	1,                   // plugin minor
	FF_MIXER,
	"Applies one layer's motion to another layer's pixels",
	"ffgl-datamosh"
);
