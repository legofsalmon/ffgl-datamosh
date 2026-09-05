#pragma once

#include <GL.h>
#include <RenderTarget.h>

#include <cstdint>
#include <vector>

namespace datamosh::test {

/// Deterministic multi-scale value noise, sampled in pixel space.
///
/// Block matching needs something to lock onto. A flat or strictly periodic
/// pattern is either unmatchable or ambiguously matchable, and would make the
/// estimator look broken when it is behaving correctly. This is aperiodic and
/// carries structure at several scales, so every pyramid level has signal.
float PatternAt( float x, float y );

/// RGBA8 image of the pattern displaced by (shiftX, shiftY) pixels.
/// Alpha is opaque, so premultiplied and straight colour are identical and the
/// tests are not measuring alpha handling by accident.
std::vector< uint8_t > MakeShiftedPattern( int width, int height, float shiftX, float shiftY );

/// Greyscale RGBA8 image of the reference pattern displaced by (shiftX, shiftY),
/// with one half of the frame lifted to `brightLo..brightLo+amp` and the other
/// to `darkLo..darkLo+amp`.
///
/// Both halves carry the SAME contrast and differ only in DC offset, which is
/// the point: the estimator sees identical structure on both sides, so the only
/// thing that differs across the split is luma and a difference in the result
/// can only be the mask. Scaling one half's contrast instead would let the
/// search's zero bias suppress motion on the dim side, and the test would be
/// measuring the estimator rather than the mask.
///
/// Greyscale, so the mask's Rec.709 luma is exactly the value written and the
/// expectation can be reasoned about without reimplementing the conversion.
std::vector< uint8_t > MakeSplitPattern( int width, int height, float shiftX, float shiftY,
                                         float brightLo, float darkLo, float amp,
                                         bool brightLeft );

/// RGBA8 image where only a band down the left-hand side moves.
///
/// `bandFraction` of the width is the reference pattern displaced by
/// (shiftX, shiftY); the rest is the same pattern standing perfectly still,
/// lifted by `stillOffset`. The point is to have a region with genuine motion
/// and a region with provably none, so a test can assert that something reached
/// the still one.
///
/// `stillOffset` exists because a truly unchanging region cannot show whether
/// it moshed: holding a pixel and refreshing it both produce the same colour
/// when the pixel never changes. Ramping the offset over a run makes the still
/// region change WITHOUT moving, which is the only way the difference between
/// held and refreshed becomes visible there.
std::vector< uint8_t > MakeMovingBandPattern( int width, int height,
                                              float shiftX, float shiftY,
                                              float bandFraction, float stillOffset );

/// Flat RGBA8 image, for cut-detection tests.
std::vector< uint8_t > MakeSolid( int width, int height, uint8_t r, uint8_t g, uint8_t b );

/// A GL texture whose contents can be replaced frame to frame, standing in for
/// a host input.
class InputTexture
{
public:
	InputTexture() = default;
	~InputTexture();

	InputTexture( const InputTexture& )            = delete;
	InputTexture& operator=( const InputTexture& ) = delete;

	bool Create( int width, int height );
	void Upload( const std::vector< uint8_t >& rgba );
	void Release();

	GLuint GetHandle() const { return textureID; }
	int    GetWidth() const  { return width; }
	int    GetHeight() const { return height; }

private:
	GLuint textureID = 0;
	int    width     = 0;
	int    height    = 0;
};

/// Reads a render target back as RGBA floats. Single-channel targets come back
/// with the unused components filled in by GL.
std::vector< float > ReadTarget( const RenderTarget& target );

/// Mean of one component across a readback.
float MeanComponent( const std::vector< float >& rgba, int component );

/// True if every value is finite. The point of the NaN tests.
bool AllFinite( const std::vector< float >& values );

/// Mean absolute difference over the interior of two full-resolution readbacks.
///
/// The frame border is excluded because content there was off-screen a frame
/// ago: no estimator can reconstruct it, and every warp smears it inward.
///
/// Shared rather than duplicated because both test files ask the same question
/// of the same buffers — "how far is this from the live image" — and two
/// implementations of that would be free to disagree.
float MeanInteriorDifference( const std::vector< float >& a, const std::vector< float >& b,
                              int width, int height, int margin );

}  // namespace datamosh::test
