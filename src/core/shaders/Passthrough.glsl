#version 410 core

// Straight copy of the host input, used when the pipeline cannot run — an
// unusable input, a failed allocation, a shader that did not compile.
//
// The failure mode of a live effect matters as much as its output. Dropping to
// the untouched clip means a problem costs the operator an effect; rendering
// black would cost them the screen.

uniform sampler2D InputTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	// The host's colour is already premultiplied and in range, so it passes
	// through untouched.
	fragColor = texture( InputTexture, uv );
}
