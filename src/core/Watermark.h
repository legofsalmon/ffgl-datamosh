#pragma once

#include "GL.h"

#include <cstdint>
#include <string>
#include <vector>

namespace datamosh {

/// The band an unlicensed copy draws across its output. The render core knows
/// nothing about licensing; it is told which words to draw, or none.
///
/// Values are the row of the glyph texture and match licence::Mark, which the
/// plugin layer asserts.
enum class Watermark : int
{
	None       = 0,
	Unlicensed = 1,  ///< DATAMOSH · UNLICENSED
	TrialEnded = 2,  ///< DATAMOSH · TRIAL ENDED
};

namespace watermark {

/// Pixel-font glyphs are 5x7 in a 6x8 cell.
constexpr int GLYPH_WIDTH  = 5;
constexpr int GLYPH_HEIGHT = 7;
constexpr int CELL_WIDTH   = 6;
constexpr int ROW_PITCH    = 8;

/// The words for a message, '*' standing for the middle dot.
const char* Text( Watermark message );

/// Width in texels of a message: its cells, less the trailing gap.
int Columns( Watermark message );

/// The glyph texture's pixels, one byte per texel, row 0 at the top of the
/// first message. Built on the CPU once, at initialisation; never per frame.
struct Atlas
{
	int                     width  = 0;
	int                     height = 0;
	std::vector< uint8_t > texels;
};
Atlas BuildAtlas();

/// The atlas as an R8 texture, with GL state put back as the host left it.
/// 0 on failure.
GLuint CreateTexture();

}  // namespace watermark

}  // namespace datamosh
