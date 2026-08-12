#pragma once

#include <ffgl/FFGLThumbnailInfo.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace datamosh {

/// Draws the plugin's browser thumbnail from scratch, no image files involved.
///
/// The SDK's own example decodes an embedded PNG, which drags libpng and zlib
/// into the build for one 160x120 picture. CFFGLThumbnailInfo takes raw pixels,
/// so generating them costs nothing and adds no dependency.
///
/// The picture is the effect explaining itself: a clean image at the top, the
/// same image below the diagonal carried sideways in macroblocks. Anyone who has
/// seen a moshed frame recognises it at thumbnail size.
namespace thumbnail {

/// Resolume draws thumbnails at 160x120, so matching it avoids a resample.
constexpr FFUInt32 WIDTH  = 160;
constexpr FFUInt32 HEIGHT = 120;

namespace detail {

inline float Hash( int x, int y, int salt )
{
	uint32_t h = static_cast< uint32_t >( x ) * 374761393u +
	             static_cast< uint32_t >( y ) * 668265263u +
	             static_cast< uint32_t >( salt ) * 2246822519u;
	h = ( h ^ ( h >> 13 ) ) * 1274126177u;
	h = h ^ ( h >> 16 );
	return static_cast< float >( h & 0xFFFFFFu ) / static_cast< float >( 0xFFFFFF );
}

inline unsigned char ToByte( float value )
{
	const float clamped = value < 0.0f ? 0.0f : ( value > 1.0f ? 1.0f : value );
	return static_cast< unsigned char >( clamped * 255.0f + 0.5f );
}

/// The undamaged picture: soft diagonal bands with a bright disc, chosen because
/// both a smear and a block displacement are obvious against them.
inline void Source( float x, float y, int palette, float& r, float& g, float& b )
{
	const float u = x / static_cast< float >( WIDTH );
	const float v = y / static_cast< float >( HEIGHT );

	const float bands = 0.5f + 0.5f * std::sin( ( u * 3.0f + v * 2.0f ) * 6.2831853f );

	const float dx   = u - 0.34f;
	const float dy   = v - 0.40f;
	const float disc = 1.0f - ( ( dx * dx + dy * dy ) * 26.0f );
	const float glow = disc > 0.0f ? disc : 0.0f;

	if( palette == 0 )
	{
		// Magenta into cyan for the effect.
		r = 0.10f + 0.75f * bands + 0.5f * glow;
		g = 0.05f + 0.20f * ( 1.0f - bands ) + 0.6f * glow;
		b = 0.35f + 0.60f * ( 1.0f - bands ) + 0.5f * glow;
	}
	else
	{
		// Amber into teal for the mixer, so the two are told apart in a list.
		r = 0.35f + 0.60f * bands + 0.5f * glow;
		g = 0.20f + 0.55f * bands + 0.6f * glow;
		b = 0.10f + 0.15f * ( 1.0f - bands ) + 0.5f * glow;
	}
}

}  // namespace detail

/// \param palette 0 for the effect, 1 for the mixer.
inline std::vector< CFFGLColor > Generate( int palette )
{
	constexpr int BLOCK = 10;

	std::vector< CFFGLColor > pixels;
	pixels.reserve( static_cast< size_t >( WIDTH ) * HEIGHT );

	for( FFUInt32 y = 0; y < HEIGHT; ++y )
	{
		for( FFUInt32 x = 0; x < WIDTH; ++x )
		{
			const int blockX = static_cast< int >( x ) / BLOCK;
			const int blockY = static_cast< int >( y ) / BLOCK;

			// Everything below this diagonal has lost its keyframe.
			const float front = static_cast< float >( y ) / HEIGHT -
			                    ( 0.30f + 0.25f * ( static_cast< float >( x ) / WIDTH ) );

			float sampleX = static_cast< float >( x );
			float sampleY = static_cast< float >( y );
			float drift   = 0.0f;

			if( front > 0.0f )
			{
				// Displacement grows with distance past the front, and each block
				// gets its own vector, which is what makes it read as blocks
				// rather than as a blur.
				const float strength = front * 60.0f;
				sampleX -= strength * ( 0.4f + detail::Hash( blockX, blockY, 1 ) );
				sampleY -= strength * 0.25f * ( detail::Hash( blockX, blockY, 2 ) - 0.5f );
				drift = strength * 0.08f;
			}

			float r = 0.0f, g = 0.0f, b = 0.0f;
			detail::Source( sampleX, sampleY, palette, r, g, b );

			if( drift > 0.0f )
			{
				// Channels pulled apart, the fringing of a block whose planes
				// disagree about where they came from. Green stays put so the
				// image is still legible through it.
				float aheadR = 0.0f, aheadG = 0.0f, aheadB = 0.0f;
				float behindR = 0.0f, behindG = 0.0f, behindB = 0.0f;
				detail::Source( sampleX + drift, sampleY, palette, aheadR, aheadG, aheadB );
				detail::Source( sampleX - drift, sampleY, palette, behindR, behindG, behindB );
				r = aheadR;
				b = behindB;
			}

			pixels.emplace_back( detail::ToByte( r ), detail::ToByte( g ), detail::ToByte( b ),
			                     static_cast< unsigned char >( 255 ) );
		}
	}

	return pixels;
}

}  // namespace thumbnail
}  // namespace datamosh
