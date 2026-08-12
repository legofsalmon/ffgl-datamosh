#include "DatamoshMixer.h"
#include "Thumbnail.h"

#include <ffgl/FFGLPluginInfo.h>
#include <ffgl/FFGLThumbnailInfo.h>

// The class lives in the header so the tests can construct and drive it. This
// file holds only what must be exactly once per binary.

// A different palette from the effect's, so the two are distinguishable in a
// browser full of thumbnails.
static CFFGLThumbnailInfo ThumbnailInfo(
	datamosh::thumbnail::WIDTH,
	datamosh::thumbnail::HEIGHT,
	datamosh::thumbnail::Generate( 1 )
);

static CFFGLPluginInfo PluginInfo(
	PluginFactory< datamosh::DatamoshMixer >,
	"DMMX",               // unique ID, maximum four characters
	"Datamosh Transplant",// name shown in Resolume's blend-mode list
	2,                    // FFGL API major
	1,                    // FFGL API minor
	0,                    // plugin major
	1,                    // plugin minor
	FF_MIXER,
	"Applies one layer's motion to another layer's pixels",
	"ffgl-datamosh"
);
