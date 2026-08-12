# Headless GL backend. EGL is the only one that works on a CI machine with no
# display server, which is where these tests actually earn their keep.
set( DATAMOSH_TEST_BACKEND "" )

if( CMAKE_SYSTEM_NAME STREQUAL "Linux" )
    find_package( PkgConfig QUIET )
    if( PkgConfig_FOUND )
        pkg_check_modules( EGL egl )
    endif()
    if( EGL_FOUND )
        set( DATAMOSH_TEST_BACKEND "EGL" )
    endif()
endif()

if( NOT DATAMOSH_TEST_BACKEND )
    find_package( glfw3 QUIET )
    if( glfw3_FOUND )
        set( DATAMOSH_TEST_BACKEND "GLFW" )
    endif()
endif()

if( NOT DATAMOSH_TEST_BACKEND )
    message( WARNING
        "No headless OpenGL backend found (EGL on Linux, glfw3 elsewhere). "
        "Skipping tests; configure with -DDATAMOSH_BUILD_TESTS=OFF to silence this." )
    return()
endif()

add_executable( datamosh_tests
    main.cpp
    test_pipeline.cpp
    test_plugin.cpp
    benchmark.cpp
    harness/GLContext.h   harness/GLContext.cpp
    harness/Synthetic.h   harness/Synthetic.cpp
    harness/TestRunner.h
)

target_link_libraries( datamosh_tests PRIVATE datamosh::core datamosh::ffgl_sdk )
target_include_directories( datamosh_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # The plugin classes live in headers so this can drive them; only the
    # CFFGLPluginInfo registration stays in the .cpp, one per binary.
    "${CMAKE_SOURCE_DIR}/src/plugins" )

if( DATAMOSH_TEST_BACKEND STREQUAL "EGL" )
    target_compile_definitions( datamosh_tests PRIVATE DATAMOSH_TEST_EGL )
    target_include_directories( datamosh_tests PRIVATE ${EGL_INCLUDE_DIRS} )
    target_link_libraries( datamosh_tests PRIVATE ${EGL_LIBRARIES} )
else()
    target_compile_definitions( datamosh_tests PRIVATE DATAMOSH_TEST_GLFW )
    target_link_libraries( datamosh_tests PRIVATE glfw )
endif()

if( MSVC )
    target_compile_options( datamosh_tests PRIVATE /W4 )
else()
    target_compile_options( datamosh_tests PRIVATE -Wall -Wextra )
endif()

add_test( NAME datamosh_tests COMMAND datamosh_tests )

message( STATUS "  test backend : ${DATAMOSH_TEST_BACKEND}" )
