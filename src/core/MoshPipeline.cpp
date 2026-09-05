#include "MoshPipeline.h"

#include <ffgl/FFGLLog.h>
#include <ffglex/FFGLScopedFBOBinding.h>
#include <ffglex/FFGLScopedShaderBinding.h>

#include "BlockMatch.glsl.h"
#include "Composite.glsl.h"
#include "Control.glsl.h"
#include "FlowPost.glsl.h"
#include "Ingest.glsl.h"
#include "Luma.glsl.h"
#include "Mosh.glsl.h"
#include "MoshCommon.glsl.h"
#include "Passthrough.glsl.h"
#include "SceneDiff.glsl.h"
#include "ScreenQuad.glsl.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace datamosh {

namespace {

/// Edge of the change map used for cut detection. Small enough to be free,
/// large enough that a cut between two clips of similar average brightness
/// still registers, and a power of two so its mip chain lands exactly on 1x1.
constexpr GLsizei SCENE_DIFF_SIZE = 64;

/// Cost per pixel of disagreeing with the predicted vector, in units of mean
/// absolute luma difference. Tuned so a ten-pixel departure from the neighbours'
/// consensus has to earn about 4% better match to be taken.
constexpr float SEARCH_LAMBDA = 0.004f;

/// Cost per pixel of any motion at all. Keeps static backgrounds reading as
/// static, which is what stops the whole frame creeping when nothing moves.
constexpr float SEARCH_ZERO_BIAS = 0.0015f;

/// Motion threshold is exposed 0..1; this is what full scale means in pixels.
constexpr float THRESHOLD_PIXEL_RANGE = 8.0f;

/// Binds textures to consecutive units for one pass and unbinds them after.
/// FFGL requires the context be handed back in a default state, and a stray
/// texture binding is exactly the kind of leak that shows up as a corrupted
/// unrelated layer three effects down the chain.
class PassTextures
{
public:
	void Add( ffglex::FFGLShader& shader, const char* uniformName, GLuint texture )
	{
		glActiveTexture( GL_TEXTURE0 + count );
		glBindTexture( GL_TEXTURE_2D, texture );
		shader.Set( uniformName, count );
		++count;
	}

	~PassTextures()
	{
		for( int unit = count - 1; unit >= 0; --unit )
		{
			glActiveTexture( GL_TEXTURE0 + unit );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		glActiveTexture( GL_TEXTURE0 );
	}

	PassTextures()                                 = default;
	PassTextures( const PassTextures& )            = delete;
	PassTextures& operator=( const PassTextures& ) = delete;

private:
	int count = 0;
};

bool CompileOne( ffglex::FFGLShader& shader, const std::string& fragmentSource, const char* name )
{
	if( shader.Compile( std::string( shaders::ScreenQuad ), fragmentSource ) )
		return true;
	FFGLLog::LogToHost( ( std::string( "datamosh: failed to compile shader " ) + name ).c_str() );
	return false;
}

GLsizei BlocksAcross( GLsizei extent, int blockSize )
{
	return std::max< GLsizei >( 1, ( extent + blockSize - 1 ) / blockSize );
}

}  // namespace

MoshPipeline::~MoshPipeline()
{
	if( initialised )
		FFGLLog::LogToHost( "datamosh: MoshPipeline destroyed without Release() - GL objects leaked" );
}

bool MoshPipeline::Initialise()
{
	if( initialised )
		return true;

	if( !CompileShaders() )
	{
		Release();
		return false;
	}

	if( !quad.Initialise() )
	{
		FFGLLog::LogToHost( "datamosh: failed to create the fullscreen quad" );
		Release();
		return false;
	}

	initialised = true;
	return true;
}

bool MoshPipeline::CompileShaders()
{
	// Mosh and Composite share the gate arithmetic, so they share its source
	// rather than each keeping a copy. MoshCommon.glsl carries the #version line
	// for both; see the comment at the top of it for why.
	const std::string common( shaders::MoshCommon );

	return CompileOne( ingestShader, shaders::Ingest, "Ingest" ) &&
	       CompileOne( lumaShader, shaders::Luma, "Luma" ) &&
	       CompileOne( sceneDiffShader, shaders::SceneDiff, "SceneDiff" ) &&
	       CompileOne( controlShader, shaders::Control, "Control" ) &&
	       CompileOne( blockMatchShader, shaders::BlockMatch, "BlockMatch" ) &&
	       CompileOne( flowPostShader, shaders::FlowPost, "FlowPost" ) &&
	       CompileOne( moshShader, common + shaders::Mosh, "Mosh" ) &&
	       CompileOne( compositeShader, common + shaders::Composite, "Composite" ) &&
	       CompileOne( passthroughShader, shaders::Passthrough, "Passthrough" );
}

void MoshPipeline::ReleaseTargets()
{
	colourTarget.Release();
	luma.Release();
	searchFlow.Release();
	flowHistory.Release();
	accum.Release();
	sceneDiff.Release();
	state.Release();

	frameWidth      = 0;
	frameHeight     = 0;
	flowWidth       = 0;
	flowHeight      = 0;
	activeBlockSize = 0;
	hasHistory      = false;
	// A resize must not leave a record of a warp on the old geometry behind a
	// composite of the new one.
	lastMosh        = MoshRecord{};
}

void MoshPipeline::Release()
{
	ReleaseTargets();

	ingestShader.FreeGLResources();
	lumaShader.FreeGLResources();
	sceneDiffShader.FreeGLResources();
	controlShader.FreeGLResources();
	blockMatchShader.FreeGLResources();
	flowPostShader.FreeGLResources();
	moshShader.FreeGLResources();
	compositeShader.FreeGLResources();
	passthroughShader.FreeGLResources();
	quad.Release();
	profiler.Release();

	initialised = false;
}

bool MoshPipeline::EnsureResources( GLsizei width, GLsizei height, int blockSize )
{
	if( width == frameWidth && height == frameHeight && blockSize == activeBlockSize &&
	    colourTarget.IsValid() )
		return true;

	frameWidth      = width;
	frameHeight     = height;
	activeBlockSize = blockSize;
	flowWidth       = BlocksAcross( width, blockSize );
	flowHeight      = BlocksAcross( height, blockSize );

	// Everything downstream assumed the old geometry, so none of it is valid.
	// Allocate() releases first, so this doubles as the resize path.
	const bool ok =
		colourTarget.Allocate( width, height, GL_RGBA16F ) &&
		// Mips on luma are the search pyramid: coarse levels resolve large
		// displacements for almost nothing.
		luma.Allocate( width, height, GL_R16F, /*withMips*/ true ) &&
		// RGBA rather than RG so the match residual rides along with the vector.
		searchFlow.Allocate( flowWidth, flowHeight, GL_RGBA16F ) &&
		// A ring rather than a ping-pong, so Motion Lag can reach back several
		// frames. Block resolution keeps the whole thing around a megabyte.
		flowHistory.Allocate( flowWidth, flowHeight, GL_RGBA16F ) &&
		// 16F, not 8-bit: this buffer is resampled into itself every frame, and
		// at 8 bits the rounding compounds into visible banding within seconds.
		accum.Allocate( width, height, GL_RGBA16F ) &&
		sceneDiff.Allocate( SCENE_DIFF_SIZE, SCENE_DIFF_SIZE, GL_R16F, /*withMips*/ true ) &&
		// 32F for one texel costs nothing and the control state needs it: the
		// beat counter is a bar count plus a fraction, and half-float mantissa
		// runs out of resolution on that within a couple of hundred bars.
		state.Allocate( 1, 1, GL_RGBA32F );

	if( !ok )
	{
		FFGLLog::LogToHost( "datamosh: could not allocate render targets" );
		// Only the targets. Tearing down the shaders and the quad here would
		// take the passthrough path with them, turning a recoverable allocation
		// failure into a permanently black layer.
		ReleaseTargets();
		return false;
	}

	// Fresh buffers hold whatever the driver left behind, which for a feedback
	// system means the first frame could seed itself with garbage.
	luma.Clear();
	searchFlow.Clear();
	flowHistory.Clear();
	accum.Clear();
	sceneDiff.Clear();
	state.Clear();

	hasHistory = false;
	LogVramBudget();
	return true;
}

void MoshPipeline::LogVramBudget() const
{
	const size_t megabytes = GetVramBytes() / ( 1024 * 1024 );
	FFGLLog::LogToHost( ( "datamosh: " + std::to_string( frameWidth ) + "x" +
	                      std::to_string( frameHeight ) + ", " + std::to_string( flowWidth ) + "x" +
	                      std::to_string( flowHeight ) + " blocks, " + std::to_string( megabytes ) +
	                      " MB of buffers" )
	                        .c_str() );
}

size_t MoshPipeline::GetVramBytes() const
{
	return colourTarget.ByteSize() + luma.ByteSize() + searchFlow.ByteSize() +
	       flowHistory.ByteSize() + accum.ByteSize() + sceneDiff.ByteSize() + state.ByteSize();
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

void MoshPipeline::PassIngest( const FrameInputs& inputs )
{
	ScopedPassTimer timer( profiler, Pass::Ingest );

	ffglex::ScopedShaderBinding shaderBinding( ingestShader.GetGLID() );
	ffglex::ScopedFBOBinding    fboBinding( colourTarget.GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
	ScopedViewport              viewport( colourTarget.GetWidth(), colourTarget.GetHeight() );

	PassTextures textures;
	textures.Add( ingestShader, "InputTexture", inputs.pixelTexture );
	ingestShader.Set( "MaxUV", inputs.pixelMaxU, inputs.pixelMaxV );

	quad.Draw();
}

void MoshPipeline::PassLuma( const FrameInputs& inputs )
{
	ScopedPassTimer timer( profiler, Pass::Luma );

	const RenderTarget& target = luma.Back();
	{
		ffglex::ScopedShaderBinding shaderBinding( lumaShader.GetGLID() );
		ffglex::ScopedFBOBinding    fboBinding( target.GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
		ScopedViewport              viewport( target.GetWidth(), target.GetHeight() );

		// Read the host's motion texture directly rather than the ingested
		// colour: in the mixer they are different inputs, and matching is done
		// on luma anyway so premultiplication does not change the result.
		PassTextures textures;
		textures.Add( lumaShader, "Source", inputs.motionTexture );
		lumaShader.Set( "MaxUV", inputs.motionMaxU, inputs.motionMaxV );

		quad.Draw();
	}
	// Must happen with the FBO unbound; this builds the search pyramid.
	target.GenerateMips();
}

void MoshPipeline::PassSceneDiff()
{
	ScopedPassTimer timer( profiler, Pass::SceneDiff );

	// Pick the mip whose resolution is closest to the change map, so each texel
	// summarises an area rather than point-sampling one pixel of detail.
	const float ratio = static_cast< float >( std::max( frameWidth, frameHeight ) ) / SCENE_DIFF_SIZE;
	const float sampleLevel = std::max( 0.0f, std::log2( std::max( 1.0f, ratio ) ) );

	{
		ffglex::ScopedShaderBinding shaderBinding( sceneDiffShader.GetGLID() );
		ffglex::ScopedFBOBinding    fboBinding( sceneDiff.GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
		ScopedViewport              viewport( sceneDiff.GetWidth(), sceneDiff.GetHeight() );

		PassTextures textures;
		textures.Add( sceneDiffShader, "CurLuma", luma.Back().GetTexture() );
		textures.Add( sceneDiffShader, "PrevLuma", luma.Front().GetTexture() );
		sceneDiffShader.Set( "SampleLevel", sampleLevel );
		sceneDiffShader.Set( "MaxUV", 1.0f, 1.0f );

		quad.Draw();
	}
	// Reduces the map to a single mean-absolute-difference texel at the top.
	sceneDiff.GenerateMips();
}

void MoshPipeline::PassControl( const MoshParams& params )
{
	ScopedPassTimer timer( profiler, Pass::Control );

	ffglex::ScopedShaderBinding shaderBinding( controlShader.GetGLID() );
	ffglex::ScopedFBOBinding    fboBinding( state.Back().GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
	ScopedViewport              viewport( 1, 1 );

	PassTextures textures;
	textures.Add( controlShader, "State", state.Front().GetTexture() );
	textures.Add( controlShader, "SceneDiff", sceneDiff.GetTexture() );

	controlShader.Set( "DiffLevel", static_cast< float >( sceneDiff.GetMipLevels() - 1 ) );
	controlShader.Set( "MoshAmount", params.moshAmount );
	controlShader.Set( "Trigger", params.trigger ? 1 : 0 );
	controlShader.Set( "Hold", params.hold ? 1 : 0 );
	controlShader.Set( "Reset", params.reset ? 1 : 0 );
	controlShader.Set( "AutoMode", static_cast< int >( params.autoMode ) );
	controlShader.Set( "Sensitivity", params.sensitivity );
	controlShader.Set( "Duration", params.duration );
	controlShader.Set( "AudioLevel", params.audioLevel );
	controlShader.Set( "AudioAmount", params.audioAmount );
	controlShader.Set( "BarPhase", params.barPhase );
	controlShader.Set( "BeatDivisor", static_cast< float >( params.beatDivisor ) );
	controlShader.Set( "DeltaTime", params.deltaTime );
	controlShader.Set( "HasHistory", hasHistory ? 1 : 0 );
	controlShader.Set( "MaxUV", 1.0f, 1.0f );

	quad.Draw();
	state.Swap();
}

void MoshPipeline::PassMotionSearch( const MoshParams& params )
{
	ScopedPassTimer timer( profiler, Pass::MotionSearch );

	const int maxLevels = luma.Front().GetMipLevels();
	const int levels    = std::max( 1, std::min( PyramidLevelsFor( params.quality ), maxLevels ) );

	ffglex::ScopedShaderBinding shaderBinding( blockMatchShader.GetGLID() );

	blockMatchShader.Set( "FlowRes", static_cast< float >( flowWidth ), static_cast< float >( flowHeight ) );
	blockMatchShader.Set( "FrameRes", static_cast< float >( frameWidth ), static_cast< float >( frameHeight ) );
	blockMatchShader.Set( "BlockSize", static_cast< float >( activeBlockSize ) );
	blockMatchShader.Set( "Lambda", SEARCH_LAMBDA );
	blockMatchShader.Set( "ZeroBias", SEARCH_ZERO_BIAS );
	blockMatchShader.Set( "MaxUV", 1.0f, 1.0f );

	const int totalIterations = levels + ( ExtraRefinementFor( params.quality ) ? 1 : 0 );

	for( int iteration = 0; iteration < totalIterations; ++iteration )
	{
		const bool isFirst = ( iteration == 0 );
		// Coarse to fine. The extra Ultra iteration stays at level 0 but halves
		// the step, buying sub-pixel accuracy where it is most visible.
		const int   level    = std::max( 0, levels - 1 - iteration );
		const bool  subPel   = ( iteration >= levels );
		const float stepPixels = subPel ? 0.5f : static_cast< float >( 1 << level );

		// The first iteration starts from last frame's conditioned field.
		// Motion is continuous, so yesterday's answer is a far better opening
		// guess than zero, and it costs nothing.
		const RenderTarget& estimate = isFirst ? flowHistory.Current() : searchFlow.Front();
		const bool hasEstimate       = isFirst ? hasHistory : true;

		const RenderTarget& target = searchFlow.Back();
		ffglex::ScopedFBOBinding fboBinding( target.GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
		ScopedViewport           viewport( target.GetWidth(), target.GetHeight() );

		PassTextures textures;
		textures.Add( blockMatchShader, "CurLuma", luma.Back().GetTexture() );
		textures.Add( blockMatchShader, "PrevLuma", luma.Front().GetTexture() );
		textures.Add( blockMatchShader, "PrevEstimate", estimate.GetTexture() );

		blockMatchShader.Set( "Level", static_cast< float >( level ) );
		blockMatchShader.Set( "Step", stepPixels / frameWidth, stepPixels / frameHeight );
		blockMatchShader.Set( "WideSearch", isFirst ? 1 : 0 );
		blockMatchShader.Set( "HasEstimate", hasEstimate ? 1 : 0 );

		quad.Draw();

		fboBinding.EndScope();
		searchFlow.Swap();
	}
}

void MoshPipeline::PassFlowPost( const MoshParams& params )
{
	ScopedPassTimer timer( profiler, Pass::FlowPost );

	const int levels = std::max( 1, std::min( PyramidLevelsFor( params.quality ),
	                                          luma.Front().GetMipLevels() ) );
	// The furthest the pyramid can legitimately reach. Anything beyond it is a
	// bad match, not motion, so this is where it gets cut off.
	maxPixels = std::min( 256.0f, std::max( 8.0f, 4.0f * ( 1 << ( levels - 1 ) ) ) );

	ffglex::ScopedShaderBinding shaderBinding( flowPostShader.GetGLID() );
	ffglex::ScopedFBOBinding    fboBinding( flowHistory.Next().GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
	ScopedViewport              viewport( flowWidth, flowHeight );

	PassTextures textures;
	textures.Add( flowPostShader, "RawFlow", searchFlow.Front().GetTexture() );
	textures.Add( flowPostShader, "PrevFlow", flowHistory.Current().GetTexture() );

	flowPostShader.Set( "FlowRes", static_cast< float >( flowWidth ), static_cast< float >( flowHeight ) );
	flowPostShader.Set( "FrameRes", static_cast< float >( frameWidth ), static_cast< float >( frameHeight ) );
	flowPostShader.Set( "Smoothing", params.motionSmoothing );
	flowPostShader.Set( "DeltaTime", params.deltaTime );
	flowPostShader.Set( "Softness", params.softness );
	flowPostShader.Set( "MaxPixels", maxPixels );
	flowPostShader.Set( "HasHistory", hasHistory ? 1 : 0 );
	flowPostShader.Set( "MaxUV", 1.0f, 1.0f );

	quad.Draw();

	fboBinding.EndScope();
	flowHistory.Advance();
}

void MoshPipeline::PassMosh( const MoshParams& params )
{
	ScopedPassTimer timer( profiler, Pass::Mosh );

	ffglex::ScopedShaderBinding shaderBinding( moshShader.GetGLID() );
	ffglex::ScopedFBOBinding    fboBinding( accum.Back().GetFBO(), ffglex::ScopedFBOBinding::RB_REVERT );
	ScopedViewport              viewport( frameWidth, frameHeight );

	PassTextures textures;
	textures.Add( moshShader, "CurColor", colourTarget.GetTexture() );
	textures.Add( moshShader, "AccumPrev", accum.Front().GetTexture() );
	// Not the current field but one from `motionLag` frames ago, so the motion
	// lands on content it does not belong to.
	textures.Add( moshShader, "Flow", flowHistory.Delayed( params.motionLag ).GetTexture() );
	textures.Add( moshShader, "State", state.Front().GetTexture() );

	moshShader.Set( "FrameRes", static_cast< float >( frameWidth ), static_cast< float >( frameHeight ) );
	moshShader.Set( "FlowRes", static_cast< float >( flowWidth ), static_cast< float >( flowHeight ) );
	moshShader.Set( "MotionGain", params.motionGain );
	moshShader.Set( "Softness", params.softness );
	moshShader.Set( "PelSnap", params.pelSnap );
	moshShader.Set( "Direction", params.invertDirection ? -1.0f : 1.0f );
	// Squared: per-frame motion on ordinary footage is mostly 0.5–3px, so a
	// linear 0–8px slider keeps the whole decision in its bottom quarter and
	// turns the rest into a wall. t² puts 0.5 at 2px instead of 4. Endpoints
	// are unchanged. Then clamped below the pyramid's reach, so no Quality
	// setting can make the gate unclearable.
	const float t               = params.motionThreshold;
	const float thresholdPixels = std::min( t * t * THRESHOLD_PIXEL_RANGE, maxPixels * 0.75f );
	moshShader.Set( "ThresholdPixels", thresholdPixels );
	// Quantise moved here from PassFlowPost: it belongs downstream of the gate,
	// so a vector that rounds away cannot close the gate with it.
	moshShader.Set( "Quantise", params.motionQuantise );
	moshShader.Set( "BlockPixels", static_cast< float >( activeBlockSize ) );
	moshShader.Set( "Decay", params.decay );
	moshShader.Set( "Corruption", params.corruption );
	moshShader.Set( "BlockRepeat", params.blockRepeat );
	moshShader.Set( "ChromaDrift", params.chromaDrift );
	moshShader.Set( "CorruptEpoch", static_cast< float >( params.corruptEpoch ) );
	moshShader.Set( "DeltaTime", params.deltaTime );
	moshShader.Set( "HasHistory", hasHistory ? 1 : 0 );
	moshShader.Set( "MaxUV", 1.0f, 1.0f );

	quad.Draw();

	// What the warp just used, for any pass that has to redraw this decision.
	// Recorded rather than re-read, because Composite is not guaranteed to run
	// in the same call as Advance.
	lastMosh.softness        = params.softness;
	lastMosh.corruption      = params.corruption;
	lastMosh.corruptEpoch    = static_cast< float >( params.corruptEpoch );
	lastMosh.thresholdPixels = thresholdPixels;
	lastMosh.decay           = params.decay;
	lastMosh.hasHistory      = hasHistory;

	fboBinding.EndScope();
	accum.Swap();
}

// ---------------------------------------------------------------------------
// Driving
// ---------------------------------------------------------------------------

bool MoshPipeline::Advance( const FrameInputs& inputs, const MoshParams& params )
{
	if( !initialised )
		return false;
	if( inputs.pixelTexture == 0 || inputs.motionTexture == 0 )
		return false;
	if( inputs.width <= 0 || inputs.height <= 0 )
		return false;

	const int blockSize = std::max( 2, params.blockSize );
	if( !EnsureResources( inputs.width, inputs.height, blockSize ) )
		return false;

	// Reset is a keyframe, so it has to discard the history and not just the
	// control level. Before this it zeroed one texel of state, and a vector
	// field frozen across a cut survived it permanently. Dropping hasHistory
	// makes the mosh pass take its first-frame path and write the live frame
	// into the accumulation buffer — which is what makes this a keyframe, and
	// why the buffer is not cleared to black: a zeroed buffer under a level
	// still ramping down is a dark flash on a panic button.
	if( params.reset )
	{
		flowHistory.Clear();
		hasHistory = false;
	}

	PassIngest( inputs );
	PassLuma( inputs );
	PassSceneDiff();
	PassControl( params );
	PassMotionSearch( params );
	PassFlowPost( params );
	PassMosh( params );

	// Only now is the previous frame's luma finished with.
	luma.Swap();

	profiler.NextFrame();

	hasHistory = true;
	return true;
}

void MoshPipeline::Composite( GLuint hostFBO, float mix )
{
	if( !initialised || !accum.IsValid() )
		return;

	// Explicitly rebind rather than trusting the scoped bindings to have
	// unwound: this is the last thing the host sees, and getting it wrong means
	// drawing into one of our own buffers instead of the composition.
	ScopedPassTimer timer( profiler, Pass::Composite );

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );

	ffglex::ScopedShaderBinding shaderBinding( compositeShader.GetGLID() );

	PassTextures textures;
	textures.Add( compositeShader, "Accum", accum.Front().GetTexture() );
	textures.Add( compositeShader, "CurColor", colourTarget.GetTexture() );
	compositeShader.Set( "Mix", mix );
	compositeShader.Set( "MaxUV", 1.0f, 1.0f );

	quad.Draw();
}

void MoshPipeline::Passthrough( GLuint hostFBO, const FrameInputs& inputs )
{
	if( inputs.pixelTexture == 0 || !passthroughShader.IsReady() )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );

	ffglex::ScopedShaderBinding shaderBinding( passthroughShader.GetGLID() );

	PassTextures textures;
	textures.Add( passthroughShader, "InputTexture", inputs.pixelTexture );
	passthroughShader.Set( "MaxUV", inputs.pixelMaxU, inputs.pixelMaxV );

	quad.Draw();
}

}  // namespace datamosh
