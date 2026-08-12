#pragma once

#include "GL.h"

namespace datamosh {

/// A colour-only framebuffer with an optional mip chain.
///
/// This deliberately replaces ffglex::FFGLFBO, for two reasons:
///
///  1. FFGLFBO::Release() leaks. Its second guard tests `depthBufferID` (already
///     zeroed by the block above) instead of `colorTextureID`, so the colour
///     texture is never deleted — a full-resolution texture leaked on every
///     resize. This effect reallocates on every composition resolution change,
///     which would make that leak load-bearing.
///  2. FFGLFBO always allocates a depth renderbuffer. None of our passes use
///     depth, and at 4K that is ~32 MB of pure waste per buffer.
///
/// It also adds what the pass graph actually needs: float internal formats and
/// a mip chain for the motion-search pyramid.
class RenderTarget
{
public:
	RenderTarget() = default;
	~RenderTarget();

	RenderTarget( const RenderTarget& )            = delete;
	RenderTarget& operator=( const RenderTarget& ) = delete;
	RenderTarget( RenderTarget&& other ) noexcept;
	RenderTarget& operator=( RenderTarget&& other ) noexcept;

	/// Allocates the texture and framebuffer. Releases any previous allocation
	/// first, so this doubles as "resize".
	///
	/// \param internalFormat  Must be colour-renderable in GL 4.1 core:
	///                        GL_R16F, GL_RG16F, GL_RGBA16F, GL_RGBA8, ...
	/// \param withMips        Allocate a full mip chain (for pyramid search and
	///                        for whole-frame reductions down to 1x1).
	bool Allocate( GLsizei width, GLsizei height, GLint internalFormat, bool withMips = false );

	void Release();

	bool IsValid() const { return fboID != 0 && textureID != 0; }

	GLuint  GetFBO() const     { return fboID; }
	GLuint  GetTexture() const { return textureID; }
	GLsizei GetWidth() const   { return width; }
	GLsizei GetHeight() const  { return height; }
	GLint   GetFormat() const  { return internalFormat; }
	bool    HasMips() const    { return hasMips; }
	/// Number of mip levels, i.e. the level index of the 1x1 top of the chain plus one.
	GLint   GetMipLevels() const { return mipLevels; }

	/// Recomputes the mip chain from level 0. No-op when the target has no mips.
	void GenerateMips() const;

	/// Clears level 0 to the given colour without disturbing the host's clear state.
	void Clear( float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 0.0f ) const;

	/// Bytes of VRAM this target occupies, mips included. Used for budget logging.
	size_t ByteSize() const;

private:
	GLuint  fboID          = 0;
	GLuint  textureID      = 0;
	GLsizei width          = 0;
	GLsizei height         = 0;
	GLint   internalFormat = 0;
	bool    hasMips        = false;
	GLint   mipLevels      = 1;
};

/// Two RenderTargets swapped each frame, for passes that read last frame's
/// result while writing this frame's.
class PingPong
{
public:
	bool Allocate( GLsizei width, GLsizei height, GLint internalFormat, bool withMips = false )
	{
		return targets[ 0 ].Allocate( width, height, internalFormat, withMips ) &&
		       targets[ 1 ].Allocate( width, height, internalFormat, withMips );
	}

	void Release()
	{
		targets[ 0 ].Release();
		targets[ 1 ].Release();
		front = 0;
	}

	bool IsValid() const { return targets[ 0 ].IsValid() && targets[ 1 ].IsValid(); }

	/// The buffer holding the current contents — read this.
	const RenderTarget& Front() const { return targets[ front ]; }
	RenderTarget&       Front()       { return targets[ front ]; }

	/// The buffer to render into — write this, then Swap().
	const RenderTarget& Back() const { return targets[ front ^ 1 ]; }
	RenderTarget&       Back()       { return targets[ front ^ 1 ]; }

	void Swap() { front ^= 1; }

	void Clear( float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 0.0f ) const
	{
		targets[ 0 ].Clear( r, g, b, a );
		targets[ 1 ].Clear( r, g, b, a );
	}

	size_t ByteSize() const { return targets[ 0 ].ByteSize() + targets[ 1 ].ByteSize(); }

private:
	RenderTarget targets[ 2 ];
	int          front = 0;
};

}  // namespace datamosh
