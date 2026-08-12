cmake_minimum_required(VERSION 3.20)
project(wxl_world_invert LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/shared.cmake")

add_library(wxl-world-invert SHARED src/Module.cpp)
wxl_apply_extension_defaults(wxl-world-invert)
target_include_directories(wxl-world-invert PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(wxl-world-invert PROPERTIES
    OUTPUT_NAME "wxl-world-invert"
    PREFIX "")

add_executable(wxl-world-invert-static-tests tests/StaticTests.cpp)
target_compile_features(wxl-world-invert-static-tests PRIVATE cxx_std_20)
target_include_directories(wxl-world-invert-static-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WXL_SDK_ROOT}/include"
    "${WXL_SDK_ROOT}/src")
if (MSVC)
    target_compile_options(wxl-world-invert-static-tests PRIVATE /W4 /WX /permissive- /EHsc)
endif()

add_executable(wxl-world-invert-abi-probe tests/AbiProbe.cpp)
target_compile_features(wxl-world-invert-abi-probe PRIVATE cxx_std_20)
target_include_directories(wxl-world-invert-abi-probe PRIVATE "${WXL_SDK_ROOT}/include")
if (MSVC)
    target_compile_options(wxl-world-invert-abi-probe PRIVATE /W4 /WX /permissive- /EHsc)
endif()
