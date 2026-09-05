#pragma once

#include "GL.h"
#include "GpuProfiler.h"
#include "MoshParams.h"
#include "RenderTarget.h"

#include <ffglex/FFGLScreenQuad.h>
#include <ffglex/FFGLShader.h>

#include <cstddef>

namespace datamosh {

/// One frame's worth of host input.
///
/// Pixels and motion are separate so the same pipeline serves both plugins: the
/// effect points both at its single input, while the mixer takes motion from one
/// layer and pixels from another to transplant one clip's movement onto another.
struct FrameInputs
{
	GLuint  pixelTexture  = 0;
	float   pixelMaxU     = 1.0f;
	float   pixelMaxV     = 1.0f;

	GLuint  motionTexture = 0;
	float   motionMaxU    = 1.0f;
	float   motionMaxV    = 1.0f;

	GLsizei width  = 0;
	GLsizei height = 0;
};

/// The whole datamosh render graph, with no FFGL host types in its interface.
///
/// Keeping it host-agnostic is what lets the headless tests drive it exactly as
/// Resolume does — which is the only way to regression-test a motion estimator,
/// since "does it look right" is not a test.
class MoshPipeline
{
public:
	MoshPipeline() = default;
	~MoshPipeline();

	MoshPipeline( const MoshPipeline& )            = delete;
	MoshPipeline& operator=( const MoshPipeline& ) = delete;

	/// Compiles shaders and allocates the fullscreen quad. Needs a current
	/// OpenGL 4.1 core context.
	bool Initialise();

	/// Frees every GL object. Must be called while the context is still alive.
	void Release();

	bool IsInitialised() const { return initialised; }

	/// Advances the simulation by one frame: motion search, control state and
	/// accumulation. Safe to skip when host time has not moved.
	/// \return false if inputs were unusable, in which case nothing changed.
	bool Advance( const FrameInputs& inputs, const MoshParams& params );

	/// Draws the current result into the framebuffer and viewport the host has
	/// bound. Does not modify the viewport.
	void Composite( GLuint hostFBO, float mix );

	/// Draws the untouched input straight through. Used when the pipeline could
	/// not run, so a failure degrades to passthrough rather than to black.
	void Passthrough( GLuint hostFBO, const FrameInputs& inputs );

	/// Forces the next Advance to treat itself as a keyframe.
	void Invalidate() { hasHistory = false; }

	/// Turns on per-pass GPU timing. Off by default and free when off.
	bool EnableProfiling() { return profiler.Enable(); }
	const GpuProfiler& GetProfiler() const { return profiler; }

	/// Total VRAM held by the intermediate buffers.
	size_t GetVramBytes() const;

	GLsizei GetWidth() const  { return frameWidth; }
	GLsizei GetHeight() const { return frameHeight; }
	GLsizei GetFlowWidth() const  { return flowWidth; }
	GLsizei GetFlowHeight() const { return flowHeight; }
	bool    HasHistory() const    { return hasHistory; }

	/// Intermediate buffers, exposed so the tests can assert on what the
	/// estimator actually produced rather than on how the result looks.
	const RenderTarget& GetFlowField() const { return flowHistory.Current(); }
	/// The field from `framesAgo` back, for asserting on Motion Lag.
	const RenderTarget& GetDelayedFlowField( int framesAgo ) const
	{
		return flowHistory.Delayed( framesAgo );
	}
	const RenderTarget& GetAccumulation() const { return accum.Front(); }
	const RenderTarget& GetControlState() const { return state.Front(); }

private:
	bool CompileShaders();
	/// Frees the render targets but keeps the shaders and quad, so the
	/// passthrough path survives an allocation failure.
	void ReleaseTargets();
	/// (Re)allocates buffers when the frame size or block size changes.
	bool EnsureResources( GLsizei width, GLsizei height, int blockSize );
	void LogVramBudget() const;

	// --- passes ---
	void PassIngest( const FrameInputs& inputs );
	void PassLuma( const FrameInputs& inputs );
	void PassSceneDiff();
	void PassControl( const MoshParams& params );
	void PassMotionSearch( const MoshParams& params );
	void PassFlowPost( const MoshParams& params );
	void PassMosh( const MoshParams& params );

	bool initialised = false;

	ffglex::FFGLShader ingestShader;
	ffglex::FFGLShader lumaShader;
	ffglex::FFGLShader sceneDiffShader;
	ffglex::FFGLShader controlShader;
	ffglex::FFGLShader blockMatchShader;
	ffglex::FFGLShader flowPostShader;
	ffglex::FFGLShader moshShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLShader passthroughShader;
	ffglex::FFGLScreenQuad quad;

	RenderTarget colourTarget;  ///< ingested pixel source, straight alpha
	PingPong     luma;          ///< front = previous frame, back = current
	PingPong     searchFlow;    ///< coarse-to-fine iteration within a frame
	FlowHistory  flowHistory;   ///< conditioned fields, last N frames
	PingPong     accum;         ///< the moshed image
	RenderTarget sceneDiff;     ///< small change map, reduced via its mips
	PingPong     state;         ///< 1x1 control state

	GpuProfiler  profiler;

	GLsizei frameWidth      = 0;
	GLsizei frameHeight     = 0;
	GLsizei flowWidth       = 0;
	GLsizei flowHeight      = 0;
	int     activeBlockSize = 0;

	/// False before the first frame and after every reallocation, when there is
	/// no previous frame to displace.
	bool hasHistory = false;

	/// How far the pyramid can legitimately reach this frame, in pixels. Set
	/// by PassFlowPost, read by PassMosh to cap the motion gate — at Quality
	/// Low the reach is 8px and so is the top of Motion Threshold, and a gate
	/// that no pixel can ever clear is a bypass indistinguishable from a dead
	/// plugin.
	float maxPixels = 8.0f;

	/// Exactly what PassMosh last used, so that a pass which has to redraw the
	/// warp's decision redraws the decision that was taken rather than a
	/// plausible re-derivation of it.
	///
	/// Composite is called with this frame's parameters, but not always after
	/// this frame's Advance: the plugin's frame gate skips Advance when host
	/// time has not moved and composites anyway. Reading `params` in Composite
	/// would then draw a gate computed from a DeltaTime no mosh pass ever used,
	/// and from a Motion Lag the operator may have moved between the two calls.
	/// Recording instead of re-reading makes that class of divergence
	/// impossible.
	///
	/// Nothing reads it yet. It lands with the shared shader source because the
	/// two arrive for the same reason, and because a record populated one
	/// release before its first reader is a record whose staleness has already
	/// been exercised by every test in the suite.
	struct MoshRecord
	{
		float softness        = 0.0f;
		float corruption      = 0.0f;
		float corruptEpoch    = 0.0f;
		float thresholdPixels = 0.0f;
		float decay           = 0.0f;
		bool  hasHistory      = false;
	} lastMosh;
};

}  // namespace datamosh
