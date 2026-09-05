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
	2,                        // FFGL API major
	1,                        // FFGL API minor
	DATAMOSH_VERSION_MAJOR,   // plugin major
	DATAMOSH_VERSION_MINOR,   // plugin minor
	FF_EFFECT,
	// The full three-component version rides in the description, which is a
	// std::string inside CFFGLPluginInfo and so has no length limit — unlike
	// PluginName above, and unlike the two integers, which cannot express a
	// patch. This is the only field that carries the whole truth to the host.
	"Simulated datamosh: melt, bloom and pixel drag driven by estimated motion"
	" (v" DATAMOSH_VERSION ")",
	"ffgl-datamosh " DATAMOSH_VERSION
);
