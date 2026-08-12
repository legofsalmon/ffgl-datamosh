#include "DatamoshPlugin.h"

#include <ffgl/FFGLPluginInfo.h>
#include <ffglquickstart/FFGLPlugin.h>

namespace datamosh {

/// The single-input build. Drops on a layer, a group, or the composition.
///
/// On the composition it is at its most useful: with Auto Mode set to On Cut it
/// detects every clip change underneath and melts through it, which is the
/// classic datamosh transition with nothing for the operator to trigger.
class DatamoshEffect : public DatamoshPlugin< ffglqs::Effect >
{
public:
	DatamoshEffect()
	{
		DeclareCommonParams();
	}

protected:
	bool GatherInputs( ProcessOpenGLStruct* pGL, FrameInputs& inputs ) override
	{
		if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return false;

		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		if( texture.Handle == 0 || texture.Width == 0 || texture.Height == 0 )
			return false;

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( texture );

		// One input drives both: the clip's own motion moshes its own pixels.
		inputs.pixelTexture  = texture.Handle;
		inputs.pixelMaxU     = maxCoords.s;
		inputs.pixelMaxV     = maxCoords.t;
		inputs.motionTexture = texture.Handle;
		inputs.motionMaxU    = maxCoords.s;
		inputs.motionMaxV    = maxCoords.t;
		inputs.width         = static_cast< GLsizei >( texture.Width );
		inputs.height        = static_cast< GLsizei >( texture.Height );
		return true;
	}
};

}  // namespace datamosh

// FFGL loads one plugin per binary: FFGL.cpp resolves everything through a
// single global plugin record, so the mixer build is a separate library.
static CFFGLPluginInfo PluginInfo(
	PluginFactory< datamosh::DatamoshEffect >,
	"DMSH",         // unique ID, maximum four characters
	"Datamosh",     // name shown in Resolume
	2,              // FFGL API major
	1,              // FFGL API minor
	0,              // plugin major
	1,              // plugin minor
	FF_EFFECT,
	"Simulated datamosh: melt, bloom and pixel drag driven by estimated motion",
	"ffgl-datamosh"
);
