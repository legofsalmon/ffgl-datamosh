# Turns .glsl files into headers holding a raw string literal, so plugin binaries
# stay self-contained (a VJ plugin must never depend on files next to the binary).
#
# Each shaders/Foo.glsl becomes generated/Foo.glsl.h declaring:
#     namespace datamosh::shaders { inline constexpr const char* Foo = R"GLSL(...)GLSL"; }

function( datamosh_embed_shaders TARGET )
    set( _gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated" )
    file( MAKE_DIRECTORY "${_gen_dir}" )

    set( _headers "" )
    foreach( _shader IN LISTS ARGN )
        get_filename_component( _abs "${_shader}" ABSOLUTE )
        get_filename_component( _name "${_shader}" NAME_WE )
        set( _out "${_gen_dir}/${_name}.glsl.h" )

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND}
                    -DINPUT=${_abs}
                    -DOUTPUT=${_out}
                    -DSYMBOL=${_name}
                    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedShaderFile.cmake"
            DEPENDS "${_abs}" "${CMAKE_SOURCE_DIR}/cmake/EmbedShaderFile.cmake"
            COMMENT "Embedding shader ${_name}.glsl"
            VERBATIM
        )
        list( APPEND _headers "${_out}" )
    endforeach()

    target_sources( ${TARGET} PRIVATE ${_headers} )
    target_include_directories( ${TARGET} PUBLIC "${_gen_dir}" )
endfunction()
