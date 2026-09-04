cmake_minimum_required(VERSION 3.24)

project(
    CheekyFoveatedDLSS
    VERSION 0.1.0
    DESCRIPTION "Foveated DLSS-SR and DLSS-NR ReShade add-on"
    LANGUAGES C CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "CheekyFoveatedDLSS supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CheekyFoveatedDLSS must be built for x64")
endif()

file(
    GLOB CHEEKY_FOVEATED_DLSS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

list(
    APPEND CHEEKY_FOVEATED_DLSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/gaze_math.cpp"
)

list(
    APPEND CHEEKY_FOVEATED_DLSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/src/buffer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/src/hook.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/src/trampoline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/src/hde/hde64.c"
)

add_library(CheekyFoveatedDLSS MODULE ${CHEEKY_FOVEATED_DLSS_SOURCES})

target_compile_features(CheekyFoveatedDLSS PRIVATE cxx_std_20)
target_include_directories(
    CheekyFoveatedDLSS
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/deps/minhook/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/shared"
)
target_compile_definitions(
    CheekyFoveatedDLSS
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

target_link_libraries(
    CheekyFoveatedDLSS
    PRIVATE
        d3d11
        d3d12
        d3dcompiler
        dxgi
        dxguid
)

set_target_properties(
    CheekyFoveatedDLSS
    PROPERTIES
        CXX_EXTENSIONS OFF
        PREFIX ""
        OUTPUT_NAME "CheekyFoveatedDLSS"
        SUFFIX ".addon64"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CheekyFoveatedDLSS
        PRIVATE
            /W4
            /permissive-
            /utf-8
    )
endif()

add_library(
    CheekyOpenXRLayer
    MODULE
        "${CMAKE_CURRENT_SOURCE_DIR}/openxr_layer/openxr_layer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shared/gaze_math.cpp"
)
target_compile_features(CheekyOpenXRLayer PRIVATE cxx_std_20)
target_include_directories(
    CheekyOpenXRLayer
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/shared"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include"
)
target_compile_definitions(
    CheekyOpenXRLayer
    PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
target_link_libraries(CheekyOpenXRLayer PRIVATE d3d11 d3d12 dxgi dxguid)
set_target_properties(
    CheekyOpenXRLayer
    PROPERTIES
        CXX_EXTENSIONS OFF
        PREFIX ""
        OUTPUT_NAME "CheekyOpenXRLayer"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
if(MSVC)
    target_compile_options(
        CheekyOpenXRLayer PRIVATE /W4 /permissive- /utf-8
    )
endif()

add_custom_command(
    TARGET CheekyOpenXRLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/openxr_layer/XR_APILAYER_CHEEKY_foveated_dlss.json"
        "$<TARGET_FILE_DIR:CheekyOpenXRLayer>/XR_APILAYER_CHEEKY_foveated_dlss.json"
)

enable_testing()
add_executable(
    CheekyTests
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/gaze_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/gaze_math.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/foveation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gaze_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12_ngx_dispatch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12_output_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics.cpp"
)
target_compile_features(CheekyTests PRIVATE cxx_std_20)
target_include_directories(
    CheekyTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/shared"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(
    CheekyTests PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
if(MSVC)
    target_compile_options(CheekyTests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME CheekyGazeTests COMMAND CheekyTests)
