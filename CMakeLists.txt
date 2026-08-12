cmake_minimum_required( VERSION 3.20 )

project( ffgl-datamosh
    VERSION 0.1.2
    DESCRIPTION "Datamosh effects for Resolume"
    LANGUAGES CXX
)

# Apple Silicon hosts cannot load x86_64 plugins and vice versa, so Resolume
# needs a universal binary. Set before any target is declared.
if( APPLE AND NOT CMAKE_OSX_ARCHITECTURES )
    set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE )
endif()
if( APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET )
    set( CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE )
endif()

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

if( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
    set( CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE )
endif()

set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

option( DATAMOSH_BUILD_TESTS "Build the headless test harness" ON )

list( APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" )
include( FFGLPlugin )
include( EmbedShaders )

add_subdirectory( src/core )
add_subdirectory( src/plugins )

if( DATAMOSH_BUILD_TESTS )
    enable_testing()
    add_subdirectory( tests )
endif()

message( STATUS "" )
message( STATUS "ffgl-datamosh ${PROJECT_VERSION}" )
message( STATUS "  build type   : ${CMAKE_BUILD_TYPE}" )
if( APPLE )
    message( STATUS "  architectures: ${CMAKE_OSX_ARCHITECTURES}" )
endif()
message( STATUS "  tests        : ${DATAMOSH_BUILD_TESTS}" )
message( STATUS "" )
