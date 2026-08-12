#version 410 core

// Hierarchical block matching — the motion estimator that stands in for the
// motion vectors a codec would have given us.
//
// One fragment per macroblock, run once per pyramid level from coarse to fine.
// Each level reads the level above as its starting guess (bilinear magnification
// upsamples it for free) and searches a small neighbourhood around it. This is
// how video encoders do it, and it is the reason the whole thing fits in a
// couple of milliseconds: a coarse level resolves large displacements cheaply,
// and the fine levels only ever refine by a pixel or two.
//
// Vector convention: `flow` is the BACKWARD vector, i.e. the current block's
// content is found in the previous frame at `uv + flow`. That is exactly what a
// P-frame stores, so warping with it reproduces motion compensation directly.

uniform sampler2D CurLuma;    // luma of this frame, with mips
uniform sampler2D PrevLuma;   // luma of the previous frame, with mips
uniform sampler2D PrevEstimate;// result of the previous (coarser) level

uniform vec2  FlowRes;     // block-grid resolution
uniform vec2  FrameRes;    // full-frame resolution in pixels
uniform float BlockSize;   // macroblock edge in pixels
uniform float Level;       // mip level to match against
uniform vec2  Step;        // candidate spacing, in uv units
uniform bool  WideSearch;  // 5x5 at the coarsest level, 3x3 when refining
uniform bool  HasEstimate; // false on the very first level
uniform float Lambda;      // cost per pixel of straying from the predicted vector
uniform float ZeroBias;    // cost per pixel of vector length, keeps stills still

in vec2 uv;
out vec4 fragColor;

// Samples per macroblock edge. 16 taps per candidate is enough to rank
// candidates reliably while keeping the inner loop small.
const int GRID = 4;

/// Regularisation, charged in pixels so the tuning holds at any resolution.
///
/// Without this the search returns whichever block matches best in isolation,
/// which in flat or noisy areas is essentially random and reads as confetti.
/// Charging for disagreement with the neighbours, and for motion at all, is
/// what a real encoder's rate term does and it is what makes the field coherent.
float Penalty( vec2 candidate, vec2 predictor )
{
	return Lambda * length( ( candidate - predictor ) * FrameRes ) +
	       ZeroBias * length( candidate * FrameRes );
}

/// Mean absolute difference between this block and the previous frame at `mv`.
float BlockCost( vec2 blockUV, vec2 mv, vec2 blockExtent )
{
	float cost = 0.0;
	for( int y = 0; y < GRID; ++y )
	{
		for( int x = 0; x < GRID; ++x )
		{
			// Lattice of sample points spanning the block, in -0.5..0.5.
			vec2 offset = ( ( vec2( x, y ) + 0.5 ) / float( GRID ) - 0.5 ) * blockExtent;
			float a = textureLod( CurLuma, blockUV + offset, Level ).r;
			float b = textureLod( PrevLuma, blockUV + offset + mv, Level ).r;
			cost += abs( a - b );
		}
	}
	return cost / float( GRID * GRID );
}

void main()
{
	vec2 blockUV     = uv;
	vec2 blockExtent = vec2( BlockSize ) / FrameRes;
	vec2 flowTexel   = 1.0 / FlowRes;

	// Starting guess: the coarser level's vector for this block.
	vec2 predictor = HasEstimate ? texture( PrevEstimate, blockUV ).xy : vec2( 0.0 );

	vec2  best     = predictor;
	float bestCost = BlockCost( blockUV, predictor, blockExtent ) + Penalty( predictor, predictor );

	// Neighbouring blocks' vectors are strong candidates — objects are larger
	// than one macroblock, so a neighbour has usually already found the right
	// answer. Testing them is far cheaper per unit of quality than widening the
	// search window, and it is what keeps the field coherent instead of confetti.
	if( HasEstimate )
	{
		vec2 neighbours[ 4 ] = vec2[ 4 ](
			texture( PrevEstimate, blockUV + vec2( -flowTexel.x, 0.0 ) ).xy,
			texture( PrevEstimate, blockUV + vec2(  flowTexel.x, 0.0 ) ).xy,
			texture( PrevEstimate, blockUV + vec2( 0.0, -flowTexel.y ) ).xy,
			texture( PrevEstimate, blockUV + vec2( 0.0,  flowTexel.y ) ).xy
		);
		for( int i = 0; i < 4; ++i )
		{
			vec2  candidate = neighbours[ i ];
			float cost      = BlockCost( blockUV, candidate, blockExtent )
			                + Penalty( candidate, predictor );
			if( cost < bestCost )
			{
				bestCost = cost;
				best     = candidate;
			}
		}
	}

	// The zero vector, always. Without it a static region can be dragged off by
	// a marginally better match in noise.
	{
		float cost = BlockCost( blockUV, vec2( 0.0 ), blockExtent )
		           + Penalty( vec2( 0.0 ), predictor );
		if( cost < bestCost )
		{
			bestCost = cost;
			best     = vec2( 0.0 );
		}
	}

	// Local search around the predictor. WideSearch is uniform across the draw,
	// so this branch is coherent and costs nothing in divergence.
	if( WideSearch )
	{
		for( int y = -2; y <= 2; ++y )
		{
			for( int x = -2; x <= 2; ++x )
			{
				vec2  candidate = predictor + vec2( x, y ) * Step;
				float cost      = BlockCost( blockUV, candidate, blockExtent )
				                + Penalty( candidate, predictor );
				if( cost < bestCost )
				{
					bestCost = cost;
					best     = candidate;
				}
			}
		}
	}
	else
	{
		for( int y = -1; y <= 1; ++y )
		{
			for( int x = -1; x <= 1; ++x )
			{
				vec2  candidate = predictor + vec2( x, y ) * Step;
				float cost      = BlockCost( blockUV, candidate, blockExtent )
				                + Penalty( candidate, predictor );
				if( cost < bestCost )
				{
					bestCost = cost;
					best     = candidate;
				}
			}
		}
	}

	// A single NaN here would be latched into the feedback buffers and never
	// clear, so it is cheaper to refuse it at the source than to detect a black
	// screen mid-show.
	if( any( isnan( best ) ) || any( isinf( best ) ) )
	{
		best     = vec2( 0.0 );
		bestCost = 1.0;
	}

	fragColor = vec4( best, bestCost, 1.0 );
}
