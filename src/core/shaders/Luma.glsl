#version 410 core

// Extracts luma for the motion search. Matching on luma rather than RGB is what
// real encoders do: it costs a third of the texture reads and the chroma planes
// add almost nothing to match quality.
//
// The target carries a full mip chain, generated after this pass, which becomes
// the search pyramid.

uniform sampler2D Source;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec3 colour = texture( Source, uv ).rgb;
	// Rec.709 luma, matching the colour space Resolume works in.
	float luma = dot( colour, vec3( 0.2126, 0.7152, 0.0722 ) );
	fragColor  = vec4( luma, 0.0, 0.0, 1.0 );
}
