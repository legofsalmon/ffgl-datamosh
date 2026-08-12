#include "RenderTarget.h"

#include <ffgl/FFGLLog.h>
#include <ffglex/FFGLScopedFBOBinding.h>
#include <ffglex/FFGLScopedTextureBinding.h>

#include <algorithm>
#include <cmath>

namespace datamosh {

namespace {

struct FormatInfo
{
	GLenum uploadFormat;
	GLenum uploadType;
	size_t bytesPerPixel;
};

/// glTexImage2D still wants a client format/type pair even when uploading null,
/// and it must be compatible with the internal format.
FormatInfo DescribeFormat( GLint internalFormat )
{
	switch( internalFormat )
	{
	case GL_R16F:    return { GL_RED,  GL_FLOAT,         2 };
	case GL_R32F:    return { GL_RED,  GL_FLOAT,         4 };
	case GL_RG16F:   return { GL_RG,   GL_FLOAT,         4 };
	case GL_RG32F:   return { GL_RG,   GL_FLOAT,         8 };
	case GL_RGBA16F: return { GL_RGBA, GL_FLOAT,         8 };
	case GL_RGBA32F: return { GL_RGBA, GL_FLOAT,        16 };
	case GL_RGBA8:
	default:         return { GL_RGBA, GL_UNSIGNED_BYTE, 4 };
	}
}

GLint MipLevelsFor( GLsizei width, GLsizei height )
{
	GLsizei largest = std::max( width, height );
	GLint   levels  = 1;
	while( largest > 1 )
	{
		largest >>= 1;
		++levels;
	}
	return levels;
}

}  // namespace

RenderTarget::~RenderTarget()
{
	// Deleting GL objects here would be a use-after-context-destruction, since
	// a plugin's members are destroyed long after the host tears the context
	// down. Ownership therefore has to be released explicitly from DeInitGL;
	// this only reports the mistake if it was not.
	if( fboID != 0 || textureID != 0 )
		FFGLLog::LogToHost( "datamosh: RenderTarget destroyed without Release() - GL objects leaked" );
}

RenderTarget::RenderTarget( RenderTarget&& other ) noexcept
{
	*this = std::move( other );
}

RenderTarget& RenderTarget::operator=( RenderTarget&& other ) noexcept
{
	if( this != &other )
	{
		Release();
		fboID          = other.fboID;
		textureID      = other.textureID;
		width          = other.width;
		height         = other.height;
		internalFormat = other.internalFormat;
		hasMips        = other.hasMips;
		mipLevels      = other.mipLevels;

		other.fboID     = 0;
		other.textureID = 0;
		other.width     = 0;
		other.height    = 0;
		other.hasMips   = false;
		other.mipLevels = 1;
	}
	return *this;
}

bool RenderTarget::Allocate( GLsizei newWidth, GLsizei newHeight, GLint newFormat, bool withMips )
{
	if( newWidth <= 0 || newHeight <= 0 )
		return false;

	// Already exactly what was asked for.
	if( IsValid() && newWidth == width && newHeight == height &&
	    newFormat == internalFormat && withMips == hasMips )
		return true;

	Release();

	width          = newWidth;
	height         = newHeight;
	internalFormat = newFormat;
	hasMips        = withMips;
	mipLevels      = withMips ? MipLevelsFor( width, height ) : 1;

	const FormatInfo info = DescribeFormat( internalFormat );

	glGenTextures( 1, &textureID );
	if( textureID == 0 )
	{
		Release();
		return false;
	}

	{
		ffglex::Scoped2DTextureBinding textureBinding( textureID );

		// glTexStorage2D would be the obvious call, but it is OpenGL 4.2 and
		// macOS stops at 4.1, so each level is specified by hand.
		GLsizei levelWidth  = width;
		GLsizei levelHeight = height;
		for( GLint level = 0; level < mipLevels; ++level )
		{
			glTexImage2D( GL_TEXTURE_2D, level, internalFormat,
			              levelWidth, levelHeight, 0,
			              info.uploadFormat, info.uploadType, nullptr );
			levelWidth  = std::max( 1, levelWidth / 2 );
			levelHeight = std::max( 1, levelHeight / 2 );
		}

		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipLevels - 1 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		                 hasMips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR );

		// Clamping matters beyond tidiness: the warp pass samples outside the
		// frame constantly, and wrapping would drag the opposite edge into shot.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}

	glGenFramebuffers( 1, &fboID );
	if( fboID == 0 )
	{
		Release();
		return false;
	}

	{
		ffglex::ScopedFBOBinding fboBinding( fboID, ffglex::ScopedFBOBinding::RB_REVERT );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureID, 0 );

		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			// Scoped binding must unwind before Release() deletes the FBO.
			fboBinding.EndScope();
			Release();
			return false;
		}
	}

	return true;
}

void RenderTarget::Release()
{
	if( fboID != 0 )
	{
		glDeleteFramebuffers( 1, &fboID );
		fboID = 0;
	}
	if( textureID != 0 )
	{
		glDeleteTextures( 1, &textureID );
		textureID = 0;
	}
	width          = 0;
	height         = 0;
	internalFormat = 0;
	hasMips        = false;
	mipLevels      = 1;
}

void RenderTarget::GenerateMips() const
{
	if( !hasMips || textureID == 0 )
		return;
	ffglex::Scoped2DTextureBinding textureBinding( textureID );
	glGenerateMipmap( GL_TEXTURE_2D );
}

void RenderTarget::Clear( float r, float g, float b, float a ) const
{
	if( fboID == 0 )
		return;

	{
		ffglex::ScopedFBOBinding fboBinding( fboID, ffglex::ScopedFBOBinding::RB_REVERT );
		// glClearBufferfv rather than glClearColor + glClear, so we never write to
		// the host's clear-colour state.
		const GLfloat colour[ 4 ] = { r, g, b, a };
		glClearBufferfv( GL_COLOR, 0, colour );
	}

	// A framebuffer clear only touches level 0. Anything that samples a coarser
	// level before the first GenerateMips would be reading whatever the driver
	// left in that memory — which for the cut detector meant a garbage reading
	// on the very first frame, latched into its running baseline.
	GenerateMips();
}

size_t RenderTarget::ByteSize() const
{
	if( !IsValid() )
		return 0;
	const size_t base = static_cast< size_t >( width ) * height * DescribeFormat( internalFormat ).bytesPerPixel;
	// A full mip chain converges on 4/3 of the base level.
	return hasMips ? ( base * 4 ) / 3 : base;
}

}  // namespace datamosh
