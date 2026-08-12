#include "DatamoshEffect.h"
#include "Thumbnail.h"

#include <ffgl/FFGLPluginInfo.h>
#include <ffgl/FFGLThumbnailInfo.h>

// The class lives in the header so the tests can construct and drive it. This
// file holds only what must be exactly once per binary.

static CFFGLThumbnailInfo ThumbnailInfo(
	datamosh::thumbnail::WIDTH,
	datamosh::thumbnail::HEIGHT,
	datamosh::thumbnail::Generate( 0 )
);

// FFGL loads one plugin per binary: FFGL.cpp resolves everything through a
// single global plugin record, so the mixer build is a separate library.
// PluginInfoStruct::PluginName is char[16] and is not null-terminated when
// full, so fifteen characters is the limit. See DatamoshMixer.cpp, where a
// 19-character name was being truncated into the adjacent field.
static constexpr char PLUGIN_NAME[] = "Datamosh";
static_assert( sizeof( PLUGIN_NAME ) <= 16,
               "FFGL PluginName is char[16] and is not null-terminated when full" );

static CFFGLPluginInfo PluginInfo(
	PluginFactory< datamosh::DatamoshEffect >,
	"DMSH",         // unique ID, maximum four characters
	PLUGIN_NAME,
	2,              // FFGL API major
	1,              // FFGL API minor
	0,              // plugin major
	1,              // plugin minor
	FF_EFFECT,
	"Simulated datamosh: melt, bloom and pixel drag driven by estimated motion",
	"ffgl-datamosh"
);
