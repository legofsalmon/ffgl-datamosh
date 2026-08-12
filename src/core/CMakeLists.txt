add_library( datamosh_core OBJECT
    GL.h              GL.cpp
    RenderTarget.h    RenderTarget.cpp
    GpuProfiler.h     GpuProfiler.cpp
    MoshParams.h
    MoshPipeline.h    MoshPipeline.cpp
)
add_library( datamosh::core ALIAS datamosh_core )

target_include_directories( datamosh_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )
target_link_libraries( datamosh_core PUBLIC datamosh::ffgl_sdk )
set_target_properties( datamosh_core PROPERTIES POSITION_INDEPENDENT_CODE ON )

# Shaders are compiled into the binary rather than loaded from disk. A plugin
# that needs files sitting next to it is a plugin that breaks the first time
# someone copies just the .dll into their Resolume folder.
datamosh_embed_shaders( datamosh_core
    shaders/ScreenQuad.glsl
    shaders/Ingest.glsl
    shaders/Luma.glsl
    shaders/SceneDiff.glsl
    shaders/Control.glsl
    shaders/BlockMatch.glsl
    shaders/FlowPost.glsl
    shaders/Mosh.glsl
    shaders/Composite.glsl
    shaders/Passthrough.glsl
)

if( MSVC )
    target_compile_options( datamosh_core PRIVATE /W4 )
else()
    target_compile_options( datamosh_core PRIVATE -Wall -Wextra )
endif()
