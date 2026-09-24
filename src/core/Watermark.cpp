#include "Watermark.h"

#include <algorithm>
#include <cstring>

namespace datamosh::watermark {

namespace {

/// One 5x7 glyph, a row per byte, the low five bits, leftmost column highest.
struct Glyph
{
	char    code;
	uint8_t rows[ GLYPH_HEIGHT ];
};

// Only the letters the two messages use. A full font would be dead weight in
// every binary for the sake of two strings.
constexpr Glyph GLYPHS[] = {
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ '*', { 0x00, 0x00, 0x0E, 0x0E, 0x0E, 0x00, 0x00 } },  // the middle dot
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

const Glyph* Find( char code )
{
	for( const Glyph& glyph : GLYPHS )
	{
		if( glyph.code == code )
			return &glyph;
	}
	return nullptr;
}

constexpr Watermark MESSAGES[] = { Watermark::Unlicensed, Watermark::TrialEnded };

}  // namespace

const char* Text( Watermark message )
{
	switch( message )
	{
	case Watermark::Unlicensed: return "DATAMOSH * UNLICENSED";
	case Watermark::TrialEnded: return "DATAMOSH * TRIAL ENDED";
	case Watermark::None:       break;
	}
	return "";
}

int Columns( Watermark message )
{
	const int length = static_cast< int >( std::strlen( Text( message ) ) );
	return length == 0 ? 0 : length * CELL_WIDTH - 1;
}

Atlas BuildAtlas()
{
	Atlas atlas;
	for( Watermark message : MESSAGES )
		atlas.width = std::max( atlas.width, Columns( message ) );
	atlas.height = static_cast< int >( sizeof( MESSAGES ) / sizeof( MESSAGES[ 0 ] ) ) * ROW_PITCH;
	atlas.texels.assign( static_cast< size_t >( atlas.width ) * atlas.height, 0 );

	for( Watermark message : MESSAGES )
	{
		const int   top  = ( static_cast< int >( message ) - 1 ) * ROW_PITCH;
		const char* text = Text( message );
		for( int index = 0; text[ index ] != '\0'; ++index )
		{
			const Glyph* glyph = Find( text[ index ] );
			if( glyph == nullptr )
				continue;
			for( int row = 0; row < GLYPH_HEIGHT; ++row )
			{
				for( int column = 0; column < GLYPH_WIDTH; ++column )
				{
					if( glyph->rows[ row ] & ( 0x10 >> column ) )
					{
						const int x = index * CELL_WIDTH + column;
						const int y = top + row;
						atlas.texels[ static_cast< size_t >( y ) * atlas.width + x ] = 255;
					}
				}
			}
		}
	}
	return atlas;
}

GLuint CreateTexture()
{
	const Atlas atlas = BuildAtlas();

	GLint previousBinding   = 0;
	GLint previousAlignment = 4;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousBinding );
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &previousAlignment );

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	// Rows of an R8 texture this width are not 4-byte aligned.
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, atlas.width, atlas.height, 0, GL_RED, GL_UNSIGNED_BYTE,
	              atlas.texels.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, previousAlignment );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousBinding ) );
	return texture;
}

}  // namespace datamosh::watermark
