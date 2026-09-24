// Straight copy of the host input, used when the pipeline cannot run — an
// unusable input, a failed allocation, a shader that did not compile — and,
// under the Lock licence policy, as the whole output of a locked instance.
//
// No #version line: MoshPipeline compiles it as "#version 410 core" plus
// Watermark.glsl plus this, so the licence mark can be drawn here too. A locked
// instance is a passthrough WITH a band on it, which is what keeps it from
// looking like a plugin that failed to load.
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
	// through untouched. ApplyMark is an identity unless a mark was asked for.
	fragColor = ApplyMark( texture( InputTexture, uv ), uv );
}
