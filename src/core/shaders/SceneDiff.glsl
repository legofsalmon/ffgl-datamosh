#version 410 core

// Renders a small map of how much the frame changed, which the control pass
// reduces to a single number to detect hard cuts.
//
// Sampling from a coarse mip rather than full resolution is deliberate: it
// averages over an area, so camera noise and fine detail do not register as
// change, while a genuine cut still lights the whole map up.

uniform sampler2D CurLuma;
uniform sampler2D PrevLuma;
uniform float     SampleLevel;  // mip level matching this target's resolution

in vec2 uv;
out vec4 fragColor;

void main()
{
	float current  = textureLod( CurLuma, uv, SampleLevel ).r;
	float previous = textureLod( PrevLuma, uv, SampleLevel ).r;
	fragColor      = vec4( abs( current - previous ), 0.0, 0.0, 1.0 );
}
