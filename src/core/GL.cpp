#include "GL.h"

#include <ffgl/FFGLLog.h>

#include <string>

namespace datamosh {

namespace {

const char* ErrorName( GLenum err )
{
	switch( err )
	{
	case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
	case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
	case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
	default:                               return "GL_UNKNOWN_ERROR";
	}
}

}  // namespace

void FlushGLErrors()
{
	// Bounded: a lost context reports GL_INVALID_OPERATION forever, and an
	// unbounded drain would hang the render thread.
	for( int i = 0; i < 32 && glGetError() != GL_NO_ERROR; ++i )
	{
	}
}

bool CheckGL( const char* context )
{
	bool clean = true;
	for( int i = 0; i < 32; ++i )
	{
		const GLenum err = glGetError();
		if( err == GL_NO_ERROR )
			break;
		clean = false;
		FFGLLog::LogToHost( ( std::string( "datamosh: " ) + ErrorName( err ) + " in " + context ).c_str() );
	}
	return clean;
}

}  // namespace datamosh
