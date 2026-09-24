// The band an unlicensed copy draws over its final output.
//
// Not a shader on its own: MoshPipeline prepends it to Composite.glsl (after
// MoshCommon.glsl, which carries the #version line) and to Passthrough.glsl
// (after a bare #version line), and each calls ApplyMark on the colour it was
// about to write. Both final passes, because under the Lock policy the output
// IS the passthrough — and a passthrough with no mark on it is exactly what a
// dead plugin looks like.
//
// Cheap on purpose: when MarkMessage is 0, which is every frame of a licensed
// copy, it is one uniform branch. When it is not, it is a handful of ALU ops
// and one texelFetch per pixel, from a glyph texture built once at
// initialisation.

uniform sampler2D MarkFont;     // R8 glyphs, one message per 8-texel row band
uniform int       MarkMessage;  // 0 none, else the message's row band + 1
uniform int       MarkColumns;  // this message's width in glyph texels
uniform vec2      MarkFrame;    // output size in pixels
uniform vec2      MarkUVScale;  // maps this pass's uv onto 0..1 of the output

vec4 ApplyMark( vec4 colour, vec2 uv )
{
	if( MarkMessage <= 0 || MarkColumns <= 0 )
		return colour;

	vec2  pixel   = uv * MarkUVScale * MarkFrame;
	float columns = float( MarkColumns );

	// Text across 60% of the width, but never taller than 9% of the height, and
	// never below one output pixel per glyph texel, or it stops being legible.
	float scale  = max( 1.0, min( 0.6 * MarkFrame.x / columns, 0.09 * MarkFrame.y / 7.0 ) );
	vec2  size   = vec2( columns, 7.0 ) * scale;
	vec2  origin = floor( 0.5 * ( MarkFrame - size ) );
	vec2  cell   = ( pixel - origin ) / scale;  // glyph texels, y up from the text's baseline

	// A dark band the full width of the frame, two glyph texels above and
	// below the text, so the words read on any footage.
	float band = step( -2.0, cell.y ) * step( cell.y, 9.0 );

	float ink = 0.0;
	if( cell.x >= 0.0 && cell.x < columns && cell.y >= 0.0 && cell.y < 7.0 )
	{
		ivec2 texel = ivec2( int( cell.x ), ( MarkMessage - 1 ) * 8 + ( 6 - int( cell.y ) ) );
		ink         = texelFetch( MarkFont, texel, 0 ).r;
	}

	// Opaque over premultiplied colour: mix toward an opaque source is exactly
	// the premultiplied "over", alpha included, so the mark shows on a
	// transparent layer too.
	colour = mix( colour, vec4( 0.0, 0.0, 0.0, 1.0 ), 0.6 * band );
	colour = mix( colour, vec4( 1.0 ), ink );
	return colour;
}
