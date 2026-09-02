#pragma once

#include "DatamoshPlugin.h"

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
public:
	DatamoshMixer()
	{
		using ffglqs::ParamOption;

		// ffglqs::Mixer's constructor has already registered a parameter called
		// "mixVal" at index 0. It would be nice to rename it and default it to
		// fully wet, but there is no API for that: SetParamInfo unconditionally
		// push_backs a new record rather than updating an existing one, so
		// "renaming" index 0 appends a phantom parameter. GetNumParams then
		// exceeds the real count, and instantiateGL — which walks every index
		// setting its default — hits the phantom, gets FF_FAIL, and destroys the
		// instance. The plugin then fails to load with no diagnostic at all.
		//
		// So mixVal is left exactly as the base class made it, and this plugin's
		// own Mix is the wet/dry control. Folding mixVal into it is not an option
		// either: its declared default is 0, and the host writes that default at
		// instantiation, which would multiply the effect away to nothing on load.

		// FFGL's mixer convention: input 0 is the destination (what is already
		// on the layers below) and input 1 is the source (this layer's clip).
		// Taking motion from this layer and pixels from below is the useful
		// default — it makes the clip you drop the mixer onto the "motion clip".
		// Grouped, or it floats above every collapsible section as an orphan.
		this->AddGrouped( "Mosh", ParamOption::Create( "Motion Source",
		                                               { { "This Layer", 0.0f }, { "Layer Below", 1.0f } },
		                                               0 ) );
		DeclareCommonParams();

		// mixVal (index 0) cannot be renamed or removed — see above — but it can
		// be grouped. SetParamGroup mutates the record in place; it is the same
		// call AddGrouped makes for every other parameter, so it cannot
		// reproduce the phantom-parameter bug that SetParamInfo's push_back
		// does. Hiding it is one more in-place call, but it waits on a check in
		// Resolume: whether the host special-cases index 0 of a mixer as the
		// blend amount. Given what happened last time anyone touched this
		// parameter, that check comes first.
		this->SetParamGroup( 0, "Mosh" );
	}

protected:
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
