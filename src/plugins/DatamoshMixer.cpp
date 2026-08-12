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

// The name shown in Resolume's blend-mode list. PluginInfoStruct::PluginName is
// char[16] and CFFGLPluginInfo's copy loop stops after 16 bytes without writing
// a terminator, so a name of 16 characters or more runs straight into
// PluginType and a host calling strlen reads past the field. "Datamosh
// Transplant" is 19 and was being shown as "Datamosh Transpl" followed by
// PluginType's low byte. Fifteen characters is the real limit, and a compile
// error is a better place to find that out than a dropdown.
static constexpr char PLUGIN_NAME[] = "Mosh Transplant";
static_assert( sizeof( PLUGIN_NAME ) <= 16,
               "FFGL PluginName is char[16] and is not null-terminated when full" );

static CFFGLPluginInfo PluginInfo(
	PluginFactory< datamosh::DatamoshMixer >,
	"DMMX",               // unique ID, maximum four characters
	PLUGIN_NAME,
	2,                    // FFGL API major
	1,                    // FFGL API minor
	0,                    // plugin major
	1,                    // plugin minor
	FF_MIXER,
	"Applies one layer's motion to another layer's pixels",
	"ffgl-datamosh"
);
