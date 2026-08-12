#include "GLContext.h"

#include <GL.h>

#include <string>

#if defined( DATAMOSH_TEST_EGL )
#	include <EGL/egl.h>
#	include <EGL/eglext.h>
#elif defined( DATAMOSH_TEST_GLFW )
#	include <GLFW/glfw3.h>
#endif

namespace datamosh::test {

namespace {

std::string GLString( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "<unavailable>";
}

/// Resolves GL entry points once a context is current.
///
/// Everywhere except macOS, FFGL.h reaches OpenGL through GLEW, so every GL call
/// in the pipeline is a function pointer that is null until glewInit runs.
/// Without this the first glCreateShader jumps to address zero.
bool InitExtensionLoader( std::string& error )
{
#if defined( __APPLE__ )
	// macOS exposes the whole 4.1 core profile directly; there is no loader.
	( void )error;
	return true;
#else
	glewExperimental = GL_TRUE;
	const GLenum status = glewInit();

	// GLEW resolves the core entry points first and only then probes the window
	// system. On a headless EGL context there is no GLX display to probe, so it
	// reports GLEW_ERROR_NO_GLX_DISPLAY even though every function this project
	// calls is already loaded. Nothing here touches GLX, so that is not an error.
	if( status != GLEW_OK && status != GLEW_ERROR_NO_GLX_DISPLAY )
	{
		error = std::string( "glewInit failed: " ) +
		        reinterpret_cast< const char* >( glewGetErrorString( status ) );
		return false;
	}

	// glewExperimental provokes a harmless GL_INVALID_ENUM that would otherwise
	// be reported against the first pass under test.
	glGetError();

	if( glCreateShader == nullptr || glGenFramebuffers == nullptr )
	{
		error = "GL entry points did not resolve";
		return false;
	}
	return true;
#endif
}

}  // namespace

GLContext::~GLContext()
{
	Destroy();
}

std::string GLContext::Describe() const
{
	return GLString( GL_VERSION ) + " / " + GLString( GL_RENDERER );
}

#if defined( DATAMOSH_TEST_EGL )

bool GLContext::Create()
{
	// Surfaceless explicitly rather than EGL_DEFAULT_DISPLAY: CI machines have
	// no display server, and the default path fails to initialise there.
	auto getPlatformDisplay = reinterpret_cast< PFNEGLGETPLATFORMDISPLAYEXTPROC >(
		eglGetProcAddress( "eglGetPlatformDisplayEXT" ) );

	EGLDisplay dpy = EGL_NO_DISPLAY;
	if( getPlatformDisplay != nullptr )
		dpy = getPlatformDisplay( EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr );
	if( dpy == EGL_NO_DISPLAY )
		dpy = eglGetDisplay( EGL_DEFAULT_DISPLAY );
	if( dpy == EGL_NO_DISPLAY )
	{
		error = "no EGL display";
		return false;
	}

	EGLint major = 0, minor = 0;
	if( !eglInitialize( dpy, &major, &minor ) )
	{
		error = "eglInitialize failed";
		return false;
	}
	display = dpy;

	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};
	EGLConfig config = nullptr;
	EGLint    count  = 0;
	if( !eglChooseConfig( dpy, configAttributes, &config, 1, &count ) || count < 1 )
	{
		error = "no suitable EGL config";
		return false;
	}

	if( !eglBindAPI( EGL_OPENGL_API ) )
	{
		error = "eglBindAPI(EGL_OPENGL_API) failed";
		return false;
	}

	// Ask for exactly what Resolume gives us on macOS. Drivers are free to hand
	// back a higher version, so this pins the floor, not the ceiling; the
	// `#version 410 core` in every shader is what actually enforces the language.
	const EGLint contextAttributes[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4,
		EGL_CONTEXT_MINOR_VERSION, 1,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE
	};
	EGLContext ctx = eglCreateContext( dpy, config, EGL_NO_CONTEXT, contextAttributes );
	if( ctx == EGL_NO_CONTEXT )
	{
		error = "could not create an OpenGL 4.1 core context";
		return false;
	}
	context = ctx;

	if( !eglMakeCurrent( dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx ) )
	{
		error = "eglMakeCurrent failed";
		return false;
	}

	return InitExtensionLoader( error );
}

void GLContext::Destroy()
{
	if( display != nullptr )
	{
		eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
		if( context != nullptr )
			eglDestroyContext( display, context );
		eglTerminate( display );
	}
	display = nullptr;
	context = nullptr;
}

#elif defined( DATAMOSH_TEST_GLFW )

bool GLContext::Create()
{
	if( !glfwInit() )
	{
		error = "glfwInit failed";
		return false;
	}

	glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, 4 );
	glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, 1 );
	glfwWindowHint( GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE );
	glfwWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE );
	glfwWindowHint( GLFW_VISIBLE, GLFW_FALSE );

	GLFWwindow* window = glfwCreateWindow( 64, 64, "datamosh tests", nullptr, nullptr );
	if( window == nullptr )
	{
		error = "could not create an OpenGL 4.1 core context";
		glfwTerminate();
		return false;
	}
	context = window;
	glfwMakeContextCurrent( window );

	return InitExtensionLoader( error );
}

void GLContext::Destroy()
{
	if( context != nullptr )
	{
		glfwDestroyWindow( static_cast< GLFWwindow* >( context ) );
		glfwTerminate();
	}
	context = nullptr;
}

#else

bool GLContext::Create()
{
	error = "no headless GL backend was compiled in";
	return false;
}

void GLContext::Destroy()
{
}

#endif

}  // namespace datamosh::test
