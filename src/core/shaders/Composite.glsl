#version 410 core

// Final pass into the host's framebuffer.
//
// Kept separate from the mosh pass so the accumulation buffer stays in straight
// alpha and unclamped: it is fed back into itself, and premultiplying or
// clamping it would compound every frame.

uniform sampler2D Accum;
uniform sampler2D CurColor;
uniform float     Mix;  // wet/dry against the untouched input

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 wet = texture( Accum, uv );
	vec4 dry = texture( CurColor, uv );

	vec4 colour = mix( dry, wet, clamp( Mix, 0.0, 1.0 ) );

	colour.a = clamp( colour.a, 0.0, 1.0 );

	// Resolume expects premultiplied colour, and expects it inside the range
	// its video engine works in.
	colour.rgb = clamp( colour.rgb * colour.a, vec3( 0.0 ), vec3( colour.a ) );

	fragColor = colour;
}
