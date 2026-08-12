#include "harness/Synthetic.h"

#include <MoshParams.h>
#include <MoshPipeline.h>
#include <RenderTarget.h>

#include <cstdio>
#include <vector>

namespace datamosh::test {

namespace {

const char* QualityName( Quality quality )
{
	switch( quality )
	{
	case Quality::Low:    return "Low";
	case Quality::Medium: return "Medium";
	case Quality::High:   return "High";
	case Quality::Ultra:  return "Ultra";
	}
	return "?";
}

/// Frames to run before reading timings. The query ring reports three frames
/// late, and the first frames do extra work allocating and clearing.
constexpr int WARMUP_FRAMES  = 12;
constexpr int MEASURE_FRAMES = 30;

void Measure( int width, int height, int blockSize, Quality quality )
{
	MoshPipeline pipeline;
	InputTexture texture;
	// Somewhere for the composite to land. A surfaceless context has no default
	// framebuffer, so passing 0 would measure an error path rather than a draw.
	RenderTarget output;
	if( !pipeline.Initialise() || !texture.Create( width, height ) ||
	    !output.Allocate( width, height, GL_RGBA8 ) )
	{
		std::printf( "  could not set up %dx%d\n", width, height );
		return;
	}
	pipeline.EnableProfiling();

	// Composite draws into whatever viewport the host has set. Standing in for
	// the host here, otherwise it renders into a zero-sized viewport and the
	// pass appears to cost nothing.
	glViewport( 0, 0, width, height );

	FrameInputs inputs;
	inputs.pixelTexture  = texture.GetHandle();
	inputs.motionTexture = texture.GetHandle();
	inputs.width         = width;
	inputs.height        = height;

	MoshParams params;
	params.blockSize       = blockSize;
	params.quality         = quality;
	params.moshAmount      = 1.0f;
	params.motionThreshold = 0.0f;

	float shift = 0.0f;
	for( int frame = 0; frame < WARMUP_FRAMES + MEASURE_FRAMES; ++frame )
	{
		texture.Upload( MakeShiftedPattern( width, height, shift, shift * 0.5f ) );
		params.frame = frame;
		pipeline.Advance( inputs, params );
		pipeline.Composite( output.GetFBO(), params.mix );
		shift += 3.0f;
	}

	const GpuProfiler& profiler = pipeline.GetProfiler();
	std::printf( "  %4dx%-4d  block %2d  %-6s  total %6.2f ms  |",
	             width, height, blockSize, QualityName( quality ),
	             profiler.GetTotalMilliseconds() );
	for( int index = 0; index < static_cast< int >( Pass::Count ); ++index )
	{
		const Pass pass = static_cast< Pass >( index );
		std::printf( " %s %.2f", PassName( pass ), profiler.GetMilliseconds( pass ) );
	}
	std::printf( "\n" );

	texture.Release();
	output.Release();
	pipeline.Release();
}

}  // namespace

void RunBenchmark()
{
	std::printf( "Per-pass GPU time. Note that a software rasteriser will report\n"
	             "numbers that bear no relation to a real GPU; run this on the\n"
	             "target hardware before drawing conclusions.\n\n" );

	const int      resolutions[][ 2 ] = { { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
	const int      blockSizes[]       = { 8, 16, 32 };
	const Quality  qualities[]        = { Quality::Low, Quality::High, Quality::Ultra };

	for( const auto& resolution : resolutions )
	{
		for( int blockSize : blockSizes )
			for( Quality quality : qualities )
				Measure( resolution[ 0 ], resolution[ 1 ], blockSize, quality );
		std::printf( "\n" );
	}
}

}  // namespace datamosh::test
