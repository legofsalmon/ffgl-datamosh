#include "GpuProfiler.h"

#include <ffgl/FFGLLog.h>

namespace datamosh {

const char* PassName( Pass pass )
{
	switch( pass )
	{
	case Pass::Ingest:       return "ingest";
	case Pass::Luma:         return "luma";
	case Pass::SceneDiff:    return "scene-diff";
	case Pass::Control:      return "control";
	case Pass::MotionSearch: return "motion-search";
	case Pass::FlowPost:     return "flow-post";
	case Pass::Damage:       return "damage";
	case Pass::Mosh:         return "mosh";
	case Pass::Composite:    return "composite";
	case Pass::Count:        break;
	}
	return "?";
}

GpuProfiler::~GpuProfiler()
{
	if( enabled )
		FFGLLog::LogToHost( "datamosh: GpuProfiler destroyed without Release() - GL objects leaked" );
}

bool GpuProfiler::Enable()
{
	if( enabled )
		return true;

	for( Slot& slot : ring )
	{
		glGenQueries( PASS_COUNT, slot.queries.data() );
		for( int pass = 0; pass < PASS_COUNT; ++pass )
		{
			if( slot.queries[ pass ] == 0 )
			{
				Release();
				return false;
			}
			slot.issued[ pass ] = false;
		}
	}

	results.fill( 0.0f );
	writeIndex = 0;
	openPass   = -1;
	enabled    = true;
	return true;
}

void GpuProfiler::Release()
{
	if( openPass >= 0 )
	{
		glEndQuery( GL_TIME_ELAPSED );
		openPass = -1;
	}

	for( Slot& slot : ring )
	{
		for( int pass = 0; pass < PASS_COUNT; ++pass )
		{
			if( slot.queries[ pass ] != 0 )
				glDeleteQueries( 1, &slot.queries[ pass ] );
			slot.queries[ pass ] = 0;
			slot.issued[ pass ]  = false;
		}
	}
	enabled = false;
}

void GpuProfiler::Begin( Pass pass )
{
	if( !enabled )
		return;

	// Only one GL_TIME_ELAPSED query can be active at a time, so an unbalanced
	// Begin closes the previous one rather than raising a GL error.
	End();

	const int index = static_cast< int >( pass );
	if( index < 0 || index >= PASS_COUNT )
		return;

	Slot& slot = ring[ writeIndex ];
	glBeginQuery( GL_TIME_ELAPSED, slot.queries[ index ] );
	slot.issued[ index ] = true;
	openPass             = index;
}

void GpuProfiler::End()
{
	if( !enabled || openPass < 0 )
		return;
	glEndQuery( GL_TIME_ELAPSED );
	openPass = -1;
}

void GpuProfiler::NextFrame()
{
	if( !enabled )
		return;

	End();
	writeIndex = ( writeIndex + 1 ) % RING_LENGTH;

	// The slot we are about to overwrite was filled RING_LENGTH frames ago, so
	// the GPU has long since finished with it and reading costs nothing.
	Slot& slot = ring[ writeIndex ];
	for( int pass = 0; pass < PASS_COUNT; ++pass )
	{
		if( !slot.issued[ pass ] )
			continue;

		GLint ready = GL_FALSE;
		glGetQueryObjectiv( slot.queries[ pass ], GL_QUERY_RESULT_AVAILABLE, &ready );
		if( ready == GL_TRUE )
		{
			GLuint64 nanoseconds = 0;
			glGetQueryObjectui64v( slot.queries[ pass ], GL_QUERY_RESULT, &nanoseconds );
			results[ pass ] = static_cast< float >( nanoseconds ) / 1.0e6f;
		}
		slot.issued[ pass ] = false;
	}
}

float GpuProfiler::GetMilliseconds( Pass pass ) const
{
	const int index = static_cast< int >( pass );
	if( index < 0 || index >= PASS_COUNT )
		return 0.0f;
	return results[ index ];
}

float GpuProfiler::GetTotalMilliseconds() const
{
	float total = 0.0f;
	for( float value : results )
		total += value;
	return total;
}

}  // namespace datamosh
