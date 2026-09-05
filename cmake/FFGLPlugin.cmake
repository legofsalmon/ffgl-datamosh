# Builds the Resolume FFGL SDK from the pinned submodule and packages our plugins
# in the shape each host expects: a .dll exporting plugMain on Windows, a .bundle
# on macOS.
#
# The SDK ships its own CMakeLists, but it calls find_package(GLEW REQUIRED) and
# expects vcpkg. It also bundles a prebuilt static GLEW for Windows x64, which is
# reproducible without any package manager, so we compile the SDK sources
# ourselves against that instead.

set( FFGL_ROOT "${CMAKE_SOURCE_DIR}/external/ffgl" )

if( NOT EXISTS "${FFGL_ROOT}/source/lib/ffgl/FFGL.h" )
    message( FATAL_ERROR
        "The FFGL SDK submodule is missing. Run:\n"
        "    git submodule update --init --recursive" )
endif()

# ---------------------------------------------------------------------------
# ffgl_sdk — the vendored SDK, compiled from source
# ---------------------------------------------------------------------------

file( GLOB_RECURSE FFGL_SDK_SOURCES
    "${FFGL_ROOT}/source/lib/ffgl/*.cpp"
    "${FFGL_ROOT}/source/lib/ffglex/*.cpp"
    "${FFGL_ROOT}/source/lib/ffglquickstart/*.cpp"
)

# An OBJECT library, not STATIC, and that distinction is load-bearing.
#
# plugMain is the only symbol a host looks up, and it lives in FFGL.cpp alongside
# nothing else anyone references — the g_CurrPluginInfo global it uses is defined
# over in FFGLPluginInfoData.cpp. Out of a static archive the linker therefore
# drops FFGL.o entirely and produces a plugin with no entry point, which loads
# nowhere and gives no diagnostic. An OBJECT library links every object
# unconditionally, on all three toolchains, with no --whole-archive juggling.
# (This is also why the SDK's own project files compile these sources straight
# into each plugin rather than into a library.)
add_library( ffgl_sdk OBJECT ${FFGL_SDK_SOURCES} )
add_library( datamosh::ffgl_sdk ALIAS ffgl_sdk )

target_include_directories( ffgl_sdk SYSTEM PUBLIC "${FFGL_ROOT}/source/lib" )
target_compile_features( ffgl_sdk PUBLIC cxx_std_17 )

set_target_properties( ffgl_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON )

if( APPLE )
    # Apple caps OpenGL at 4.1, so there is no GLEW and no extension loader to
    # speak of — <OpenGL/gl3.h> gives us the whole 4.1 core profile directly.
    target_compile_definitions( ffgl_sdk PUBLIC
        TARGET_OS_MAC=1
        GL_SILENCE_DEPRECATION
    )
    target_link_libraries( ffgl_sdk PUBLIC
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
elseif( WIN32 )
    target_compile_definitions( ffgl_sdk PUBLIC
        GLEW_STATIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    target_include_directories( ffgl_sdk SYSTEM PUBLIC
        "${FFGL_ROOT}/deps/glew-2.1.0/include" )
    target_link_libraries( ffgl_sdk PUBLIC
        "${FFGL_ROOT}/deps/glew-2.1.0/lib/Release/x64/glew32s.lib"
        opengl32
    )
else()
    # Linux is not a Resolume target, but it is where the headless tests run in CI.
    find_package( OpenGL REQUIRED )
    find_package( GLEW REQUIRED )
    target_link_libraries( ffgl_sdk PUBLIC GLEW::GLEW OpenGL::GL )
endif()

# The SDK is third-party code; its warnings are not ours to fix.
if( MSVC )
    target_compile_options( ffgl_sdk PRIVATE /W0 )
else()
    target_compile_options( ffgl_sdk PRIVATE -w )
endif()

# ---------------------------------------------------------------------------
# datamosh_add_plugin( <target> SOURCES <files...> )
#
# One FFGL binary per plugin. This is forced by the SDK: FFGL.cpp dispatches
# through a single global g_CurrPluginInfo, so a binary can only ever advertise
# one plugin to the host.
# ---------------------------------------------------------------------------

function( datamosh_add_plugin TARGET )
    cmake_parse_arguments( ARG "" "DESCRIPTION" "SOURCES" ${ARGN} )

    if( APPLE )
        add_library( ${TARGET} MODULE ${ARG_SOURCES} )
        set_target_properties( ${TARGET} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/PluginInfo.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${TARGET}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.ffgl-datamosh.${TARGET}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        )
        # plugMain has no visibility attribute on macOS, so hiding symbols by
        # default would make the plugin unloadable.
        set_target_properties( ${TARGET} PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
        )
    else()
        add_library( ${TARGET} SHARED ${ARG_SOURCES} )
        set_target_properties( ${TARGET} PROPERTIES PREFIX "" )
        if( WIN32 )
            # FFGL.h already marks plugMain dllexport, but hosts look the symbol
            # up by exact name, so the .def keeps it undecorated for certain.
            target_sources( ${TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/cmake/FFGLPlugin.def" )

            # A version resource, so Explorer's Properties -> Details can answer
            # "which build is this?" on an installed DLL. macOS has answered it
            # since 0.1 through the bundle's Info.plist; this closes the gap on
            # the platform that had nothing. A .res contributes no symbols, so
            # it cannot disturb the OBJECT-library arrangement above that keeps
            # plugMain from being discarded.
            set( DATAMOSH_RC_TARGET "${TARGET}" )
            set( DATAMOSH_RC_DESCRIPTION "${ARG_DESCRIPTION}" )
            set( _rc "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}Version.rc" )
            configure_file( "${CMAKE_SOURCE_DIR}/cmake/DatamoshVersion.rc.in" "${_rc}" @ONLY )
            target_sources( ${TARGET} PRIVATE "${_rc}" )
        endif()
    endif()

    # Both object libraries explicitly: CMake does not propagate objects
    # transitively through an OBJECT library, only usage requirements.
    target_link_libraries( ${TARGET} PRIVATE datamosh::core datamosh::ffgl_sdk )

    install( TARGETS ${TARGET}
        LIBRARY DESTINATION "."
        BUNDLE  DESTINATION "." )
endfunction()

# ---------------------------------------------------------------------------
# Convenience: `cmake --install build` drops plugins straight into Resolume.
# ---------------------------------------------------------------------------

if( NOT DEFINED DATAMOSH_INSTALL_TO_RESOLUME )
    set( DATAMOSH_INSTALL_TO_RESOLUME OFF CACHE BOOL
         "Install built plugins into the user's Resolume Extra Effects folder" )
endif()

if( DATAMOSH_INSTALL_TO_RESOLUME )
    if( APPLE )
        set( CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
             CACHE PATH "" FORCE )
    elseif( WIN32 )
        set( CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/Documents/Resolume Arena/Extra Effects"
             CACHE PATH "" FORCE )
    endif()
endif()
