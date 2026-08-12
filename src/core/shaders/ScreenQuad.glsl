#version 410 core

// Shared vertex stage for every pass. ffglex::FFGLScreenQuad supplies
// location 0 = position, location 1 = uv in 0..1 with v increasing upward.

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

// Host input textures can be a sub-rectangle of a larger hardware texture, so
// their usable range is Width/HardwareWidth rather than 1.0. Our own render
// targets are exact-size, and pass 1.0 here.
uniform vec2 MaxUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV * MaxUV;
}
