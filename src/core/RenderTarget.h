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

/// A ring of render targets keeping the last N frames of a buffer.
///
/// Exists for the motion field, so the warp can be fed a vector field from
/// several frames ago rather than the current one. That mismatch is the point:
/// an estimator this accurate reconstructs the frame almost exactly, which is
/// correct and also far too clean to read as datamosh. Applying motion to
/// content it does not belong to is what a decoder does when its reference
/// frames are wrong, and it is where the characteristic smearing comes from.
///
/// Only ever holds block-resolution buffers, so the whole ring costs about a
/// megabyte at 1080p.
class FlowHistory
{
public:
	static constexpr int LENGTH = 16;

	bool Allocate( GLsizei width, GLsizei height, GLint internalFormat )
	{
		for( RenderTarget& target : ring )
		{
			if( !target.Allocate( width, height, internalFormat ) )
				return false;
		}
		head = 0;
		return true;
	}

	void Release()
	{
		for( RenderTarget& target : ring )
			target.Release();
		head = 0;
	}

	bool IsValid() const { return ring[ 0 ].IsValid(); }

	void Clear() const
	{
		for( const RenderTarget& target : ring )
			target.Clear();
	}

	/// The field written most recently.
	const RenderTarget& Current() const { return ring[ head ]; }

	/// Where the next field should be rendered. Call Advance() afterwards.
	const RenderTarget& Next() const { return ring[ ( head + 1 ) % LENGTH ]; }

	void Advance() { head = ( head + 1 ) % LENGTH; }

	/// The field from `framesAgo` frames back, clamped to what the ring holds.
	const RenderTarget& Delayed( int framesAgo ) const
	{
		const int clamped = framesAgo < 0 ? 0 : ( framesAgo > LENGTH - 1 ? LENGTH - 1 : framesAgo );
		return ring[ ( head - clamped + LENGTH ) % LENGTH ];
	}

	size_t ByteSize() const
	{
		size_t total = 0;
		for( const RenderTarget& target : ring )
			total += target.ByteSize();
		return total;
	}

private:
	RenderTarget ring[ LENGTH ];
	int          head = 0;
};

}  // namespace datamosh
