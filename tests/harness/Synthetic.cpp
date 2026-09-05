#include "Synthetic.h"

#include <ffglex/FFGLScopedFBOBinding.h>
#include <ffglex/FFGLScopedTextureBinding.h>

#include <cmath>

namespace datamosh::test {

namespace {

/// Stable hash on a lattice point. Not high quality, but it must give the same
/// answer on every platform or the golden expectations drift.
float LatticeValue( int x, int y, int salt )
{
	uint32_t h = static_cast< uint32_t >( x ) * 374761393u +
	             static_cast< uint32_t >( y ) * 668265263u +
	             static_cast< uint32_t >( salt ) * 2246822519u;
	h = ( h ^ ( h >> 13 ) ) * 1274126177u;
	h = h ^ ( h >> 16 );
	return static_cast< float >( h & 0xFFFFFFu ) / static_cast< float >( 0xFFFFFF );
}

float SmoothStep( float t )
{
	return t * t * ( 3.0f - 2.0f * t );
}

float ValueNoise( float x, float y, float cellSize, int salt )
{
	const float sx = x / cellSize;
	const float sy = y / cellSize;

	const float fx = std::floor( sx );
	const float fy = std::floor( sy );
	const int   ix = static_cast< int >( fx );
	const int   iy = static_cast< int >( fy );

	const float tx = SmoothStep( sx - fx );
	const float ty = SmoothStep( sy - fy );

	const float v00 = LatticeValue( ix, iy, salt );
	const float v10 = LatticeValue( ix + 1, iy, salt );
	const float v01 = LatticeValue( ix, iy + 1, salt );
	const float v11 = LatticeValue( ix + 1, iy + 1, salt );

	const float top    = v00 + ( v10 - v00 ) * tx;
	const float bottom = v01 + ( v11 - v01 ) * tx;
	return top + ( bottom - top ) * ty;
}

uint8_t ToByte( float value )
{
	const float clamped = value < 0.0f ? 0.0f : ( value > 1.0f ? 1.0f : value );
	return static_cast< uint8_t >( clamped * 255.0f + 0.5f );
}

}  // namespace

float PatternAt( float x, float y )
{
	return 0.55f * ValueNoise( x, y, 24.0f, 1 ) +
	       0.30f * ValueNoise( x, y, 9.0f, 2 ) +
	       0.15f * ValueNoise( x, y, 4.0f, 3 );
}

std::vector< uint8_t > MakeShiftedPattern( int width, int height, float shiftX, float shiftY )
{
	std::vector< uint8_t > image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			// Sampling the pattern at (x - shift) moves the content by +shift.
			const float base = PatternAt( x - shiftX, y - shiftY );
			// Slightly different phases per channel so a channel swap would show.
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ index + 0 ] = ToByte( base );
			image[ index + 1 ] = ToByte( base * 0.9f + 0.05f );
			image[ index + 2 ] = ToByte( base * 0.8f + 0.10f );
			image[ index + 3 ] = 255;
		}
	}
	return image;
}

std::vector< uint8_t > MakeSolid( int width, int height, uint8_t r, uint8_t g, uint8_t b )
{
	std::vector< uint8_t > image( static_cast< size_t >( width ) * height * 4 );
	for( size_t index = 0; index < image.size(); index += 4 )
	{
		image[ index + 0 ] = r;
		image[ index + 1 ] = g;
		image[ index + 2 ] = b;
		image[ index + 3 ] = 255;
	}
	return image;
}

InputTexture::~InputTexture()
{
	// Unlike the plugin's own resources this is torn down while the test's
	// context is still current, so releasing here is safe and convenient.
	Release();
}

bool InputTexture::Create( int newWidth, int newHeight )
{
	Release();
	width  = newWidth;
	height = newHeight;

	glGenTextures( 1, &textureID );
	if( textureID == 0 )
		return false;

	ffglex::Scoped2DTextureBinding binding( textureID );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	return true;
}

void InputTexture::Upload( const std::vector< uint8_t >& rgba )
{
	if( textureID == 0 )
		return;
	ffglex::Scoped2DTextureBinding binding( textureID );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
}

void InputTexture::Release()
{
	if( textureID != 0 )
	{
		glDeleteTextures( 1, &textureID );
		textureID = 0;
	}
	width  = 0;
	height = 0;
}

std::vector< float > ReadTarget( const RenderTarget& target )
{
	if( !target.IsValid() )
		return {};

	std::vector< float > pixels( static_cast< size_t >( target.GetWidth() ) * target.GetHeight() * 4 );
	ffglex::ScopedFBOBinding binding( target.GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.GetWidth(), target.GetHeight(), GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

float MeanComponent( const std::vector< float >& rgba, int component )
{
	if( rgba.empty() )
		return 0.0f;
	double total = 0.0;
	size_t count = 0;
	for( size_t index = component; index < rgba.size(); index += 4 )
	{
		total += rgba[ index ];
		++count;
	}
	return count == 0 ? 0.0f : static_cast< float >( total / count );
}

bool AllFinite( const std::vector< float >& values )
{
	for( float value : values )
	{
		if( std::isnan( value ) || std::isinf( value ) )
			return false;
	}
	return true;
}

float MeanInteriorDifference( const std::vector< float >& a, const std::vector< float >& b,
                              int width, int height, int margin )
{
	if( a.size() != b.size() || a.empty() )
		return -1.0f;
	double total = 0.0;
	int    count = 0;
	for( int y = margin; y < height - margin; ++y )
	{
		for( int x = margin; x < width - margin; ++x )
		{
			const size_t index = ( static_cast< size_t >( y ) * width + x ) * 4;
			for( int channel = 0; channel < 3; ++channel )
				total += std::fabs( a[ index + channel ] - b[ index + channel ] );
			count += 3;
		}
	}
	return count == 0 ? -1.0f : static_cast< float >( total / count );
}

}  // namespace datamosh::test
