# Script-mode helper invoked by datamosh_embed_shaders. Expects INPUT, OUTPUT, SYMBOL.
file( READ "${INPUT}" _contents )

# The delimiter must not appear in the shader source, or the raw literal terminates early.
if( _contents MATCHES "\\)GLSL\"" )
    message( FATAL_ERROR "${INPUT} contains the raw-string delimiter )GLSL\" and cannot be embedded" )
endif()

file( WRITE "${OUTPUT}"
"// Generated from ${SYMBOL}.glsl by cmake/EmbedShaderFile.cmake. Do not edit.
#pragma once

namespace datamosh {
namespace shaders {

inline constexpr const char* ${SYMBOL} = R\"GLSL(${_contents})GLSL\";

}  // namespace shaders
}  // namespace datamosh
" )
