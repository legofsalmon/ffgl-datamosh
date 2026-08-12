#pragma once

// FFGL.h picks the right OpenGL header per platform: GLEW on Windows,
// <OpenGL/gl3.h> on macOS. Everything here targets the OpenGL 4.1 core profile,
// because that is Apple's hard ceiling and therefore the plugin's ceiling:
// no compute shaders, no SSBOs, no image load/store.
#include <ffgl/FFGL.h>

namespace datamosh {

/// Restores the viewport on scope exit.
///
/// ffglex::ScopedFBOBinding restores the framebuffer binding but leaves the
/// viewport alone. Since our passes render into reduced-size targets, returning
/// to the host without putting the viewport back makes the host draw into a
/// corner of its own framebuffer.
class ScopedViewport
{
public:
	ScopedViewport( GLint x, GLint y, GLsizei width, GLsizei height )
	{
		glGetIntegerv( GL_VIEWPORT, previous );
		glViewport( x, y, width, height );
	}
	ScopedViewport( GLsizei width, GLsizei height ) :
		ScopedViewport( 0, 0, width, height )
	{
	}
	~ScopedViewport()
	{
		glViewport( previous[ 0 ], previous[ 1 ], previous[ 2 ], previous[ 3 ] );
	}

	ScopedViewport( const ScopedViewport& )            = delete;
	ScopedViewport& operator=( const ScopedViewport& ) = delete;

private:
	GLint previous[ 4 ] = { 0, 0, 0, 0 };
};

/// Drains the GL error queue. Errors are latched until read, so a stale error
/// from the host would otherwise be reported against our first check.
void FlushGLErrors();

/// Logs and clears any GL error, tagged with `context`. Returns true if clean.
bool CheckGL( const char* context );

}  // namespace datamosh
