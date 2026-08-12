#version 410 core

// Normalises a host input texture into our own exact-size RGBA16F buffer.
//
// Doing this once, up front, removes two whole classes of bug from every later
// pass: the MaxUV sub-rectangle (handled in the vertex stage) and Resolume's
// premultiplied alpha. After this pass, uv is honestly 0..1 and colour is
// straight, so warping and blending behave the way the maths assumes.

uniform sampler2D InputTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 colour = texture( InputTexture, uv );

	// Un-premultiply. Warping premultiplied colour drags alpha-weighted values
	// across edges and darkens the smear.
	if( colour.a > 0.0 )
		colour.rgb /= colour.a;

	fragColor = colour;
}
