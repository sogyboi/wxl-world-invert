# Shared guardrails for the isolated client-extension build.
if (NOT WIN32)
    message(FATAL_ERROR "WarcraftXL client extensions are built on Windows.")
endif()

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The WoW 3.3.5a client is 32-bit. Configure with -A Win32.")
endif()

if (NOT DEFINED WXL_SDK_ROOT OR WXL_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR "Set -DWXL_SDK_ROOT=<pinned ABI-1.1 SDK path>.")
endif()

get_filename_component(WXL_SDK_ROOT "${WXL_SDK_ROOT}" ABSOLUTE)
foreach(_wxl_required_file
    include/wxl/PluginApi.h
    include/wxl/EventScript.hpp
    src/engine/events/Event.hpp)
    if (NOT EXISTS "${WXL_SDK_ROOT}/${_wxl_required_file}")
        message(FATAL_ERROR "Not an ABI-1.1 WXL SDK: missing ${_wxl_required_file} under ${WXL_SDK_ROOT}")
    endif()
endforeach()

function(wxl_apply_extension_defaults target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WXL_EXTENSION=1)
    target_include_directories(${target_name} PRIVATE
        "${WXL_SDK_ROOT}/include"
        "${WXL_SDK_ROOT}/src")
    if (MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
endfunction()
