# Licence checking, shared by both plugins and the tests.
#
# An OBJECT library for the same reason as the FFGL SDK and the core: its
# objects are compiled straight into each plugin binary, so there is nothing
# for a customer to install and no archive for a linker to prune.

set( MONOCYPHER_ROOT "${CMAKE_SOURCE_DIR}/external/monocypher" )

add_library( datamosh_licence OBJECT
    sdk/licence.hpp   sdk/licence.cpp
    Json.h            Json.cpp
    Licence.h         Licence.cpp
    Wire.h            Wire.cpp
    Store.h           Store.cpp
    Service.h         Service.cpp
    Runtime.h         Runtime.cpp
    Platform.h
    "${MONOCYPHER_ROOT}/monocypher.c"
    "${MONOCYPHER_ROOT}/monocypher-ed25519.c"
)
add_library( datamosh::licence ALIAS datamosh_licence )

if( WIN32 )
    target_sources( datamosh_licence PRIVATE PlatformWindows.cpp )
    target_link_libraries( datamosh_licence PUBLIC winhttp shell32 ole32 advapi32 )
elseif( APPLE )
    target_sources( datamosh_licence PRIVATE PlatformApple.mm )
    set_source_files_properties( PlatformApple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc" )
    target_link_libraries( datamosh_licence PUBLIC
        "-framework Foundation"
        "-framework IOKit"
        "-framework CoreFoundation" )
else()
    target_sources( datamosh_licence PRIVATE PlatformLinux.cpp )
    find_package( Threads REQUIRED )
    target_link_libraries( datamosh_licence PUBLIC Threads::Threads ${CMAKE_DL_LIBS} )
endif()

target_include_directories( datamosh_licence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )
# PUBLIC so the tests can sign their own tokens with Monocypher.
target_include_directories( datamosh_licence SYSTEM PUBLIC "${MONOCYPHER_ROOT}" )

# The build's date, which the licence's update entitlement (`maintUntil`) is
# measured against. Baked in rather than read at run time, because anything
# read at run time can be edited. Configure time, which honours
# SOURCE_DATE_EPOCH for a reproducible build; a release is configured fresh in
# CI, so its date is the day it was built. Override with -DDATAMOSH_BUILD_DATE.
set( DATAMOSH_BUILD_DATE "" CACHE STRING "Unix time this build is dated; empty = configure time" )
if( DATAMOSH_BUILD_DATE STREQUAL "" )
    string( TIMESTAMP _datamosh_build_date "%s" UTC )
else()
    set( _datamosh_build_date "${DATAMOSH_BUILD_DATE}" )
endif()

# Empty ships the live key compiled into sdk/licence.hpp. Anything else
# replaces it — a test deployment's key, or a non-key to build a copy that
# checks nothing and therefore restricts nothing.
set( DATAMOSH_LICENCE_PUBLIC_KEY "" CACHE STRING "Licence verifying key (hex); empty = the live key" )

target_compile_definitions( datamosh_licence PRIVATE
    LICENCE_BACKEND_MONOCYPHER
    DATAMOSH_BUILD_DATE=${_datamosh_build_date}LL
    DATAMOSH_VERSION="${PROJECT_VERSION}"
)
if( NOT DATAMOSH_LICENCE_PUBLIC_KEY STREQUAL "" )
    target_compile_definitions( datamosh_licence PRIVATE
        DATAMOSH_LICENCE_PUBLIC_KEY="${DATAMOSH_LICENCE_PUBLIC_KEY}" )
endif()

set_target_properties( datamosh_licence PROPERTIES POSITION_INDEPENDENT_CODE ON )

# Monocypher is third-party code; its warnings are not ours to fix.
set_source_files_properties(
    "${MONOCYPHER_ROOT}/monocypher.c"
    "${MONOCYPHER_ROOT}/monocypher-ed25519.c"
    PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>" )

if( MSVC )
    target_compile_options( datamosh_licence PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4> )
else()
    target_compile_options( datamosh_licence PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra> )
endif()

message( STATUS "  licence      : build date ${_datamosh_build_date}" )
