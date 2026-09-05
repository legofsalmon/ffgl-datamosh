add_library( datamosh_core OBJECT
    GL.h              GL.cpp
    RenderTarget.h    RenderTarget.cpp
    GpuProfiler.h     GpuProfiler.cpp
    MoshParams.h
    MoshPipeline.h    MoshPipeline.cpp
)
add_library( datamosh::core ALIAS datamosh_core )

target_include_directories( datamosh_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )

# The version, compiled in from the one place it is declared. Both plugins used
# to hand CFFGLPluginInfo a hardcoded 0, 1 while the project was at 0.2.0 — a
# second copy of a number is a second copy that goes stale, and that one had.
# The version, compiled in from the one place it is declared. Both plugins used
# to hand CFFGLPluginInfo a hardcoded 0, 1 while the project was at 0.2.0 — a
# second copy of a number is a second copy that goes stale, and that one had.
#
# The two integers are deliberately just MAJOR and MINOR. FFGL's extended info
# has exactly two version fields and documents them as the numbers either side
# of the dot, so there is nowhere honest to put the patch. Encoding it as
# MINOR*100+PATCH was considered and rejected: it makes 0.3.0 and 0.30.0 both
# report 300, which is a collision that arrives silently at some future
# release. The full three-component truth goes where there is no size limit —
# the Description string, the Windows version resource, the macOS Info.plist
# and the host log.
target_compile_definitions( datamosh_core PUBLIC
    DATAMOSH_VERSION="${PROJECT_VERSION}"
    DATAMOSH_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DATAMOSH_VERSION_MINOR=${PROJECT_VERSION_MINOR}
)
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
    shaders/DamageSpread.glsl
    shaders/MoshCommon.glsl
    shaders/Mosh.glsl
    shaders/Composite.glsl
    shaders/Passthrough.glsl
)

if( MSVC )
    target_compile_options( datamosh_core PRIVATE /W4 )
else()
    target_compile_options( datamosh_core PRIVATE -Wall -Wextra )
endif()
