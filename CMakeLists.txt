cmake_minimum_required(VERSION 3.24)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "shared/version.h")
file(STRINGS "shared/version.h" CHEEKY_VERSION_LINE REGEX "^#define CHEEKY_VERSION ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" CHEEKY_VERSION "${CHEEKY_VERSION_LINE}")

project(
    CheekyFoveatedDLSS
    VERSION ${CHEEKY_VERSION}
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
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/include"
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
        shell32
        version
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
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/eye_calibration_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/openxr_calibration_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eye_calibration.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eye_calibration_d3d12.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/d3d12_composite_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/motion_resample_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/crop_motion.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support_summary_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/nr_processing_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/nr_lifetime_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_nr_lifetime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_nr_input.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared/gaze_math.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/foveation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gaze_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12_ngx_dispatch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12_output_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dlss_nr_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gaze_foveation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp"
)
add_dependencies(CheekyTests CheekyOpenXRLayer)
target_link_libraries(CheekyTests PRIVATE d3d11 d3d12 dxgi d3dcompiler)
target_compile_features(CheekyTests PRIVATE cxx_std_20)
target_compile_definitions(CheekyTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
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
add_test(NAME CheekyNrLifetimeTests COMMAND CheekyTests --nr-lifetime)
add_test(NAME CheekyNrLifetimeTestsNoDebugLayer COMMAND CheekyTests --nr-lifetime)
set_tests_properties(CheekyNrLifetimeTestsNoDebugLayer PROPERTIES
    ENVIRONMENT "CHEEKY_NR_TEST_NO_DEBUG_LAYER=1")

add_executable(CheekyNrObserverTests
    tests/nr_observer_tests.cpp tests/nr_lifetime_tests.cpp tests/d3d12_composite_tests.cpp
    src/dlss_nr_lifetime.cpp src/dlss_nr_input.cpp src/dlss_nr_contract.cpp
    src/d3d12_output_contract.cpp src/foveation.cpp src/settings.cpp src/graphics_observer.cpp
    third_party/reshade/deps/minhook/src/buffer.c
    third_party/reshade/deps/minhook/src/hook.c
    third_party/reshade/deps/minhook/src/trampoline.c
    third_party/reshade/deps/minhook/src/hde/hde64.c)
target_compile_features(CheekyNrObserverTests PRIVATE cxx_std_20)
target_compile_definitions(CheekyNrObserverTests PRIVATE CHEEKY_NR_NATIVE_OBSERVER WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(CheekyNrObserverTests PRIVATE src shared third_party/reshade/deps/minhook/include)
target_link_libraries(CheekyNrObserverTests PRIVATE d3d11 d3d12 dxgi d3dcompiler)
set_target_properties(CheekyNrObserverTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC)
    target_compile_options(CheekyNrObserverTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
add_test(NAME CheekyNrObserverTests COMMAND CheekyNrObserverTests)
add_test(NAME CheekyNrObserverTestsNoDebugLayer COMMAND CheekyNrObserverTests)
set_tests_properties(CheekyNrObserverTestsNoDebugLayer PROPERTIES
    ENVIRONMENT "CHEEKY_NR_TEST_NO_DEBUG_LAYER=1")

option(CHEEKY_BUILD_UEVR "Build the UEVR plugin, resident runtime and host harness" ON)
if(CHEEKY_BUILD_UEVR)
    set(CHEEKY_RUNTIME_SOURCES ${CHEEKY_FOVEATED_DLSS_SOURCES})
    list(FILTER CHEEKY_RUNTIME_SOURCES EXCLUDE REGEX "/(addon|support_report)\\.cpp$")
    add_library(CheekyFoveatedDLSSRuntime MODULE ${CHEEKY_RUNTIME_SOURCES}
        uevr/runtime.cpp uevr/settings_io.cpp)
    add_library(CheekyFoveatedDLSSUEVR MODULE uevr/plugin.cpp)
    add_executable(CheekyUEVRTests tests/uevr_tests.cpp tests/late_attach_tests.cpp uevr/settings_io.cpp src/settings.cpp src/foveation.cpp)
    add_library(CheekyFakeNGX MODULE tests/fake_ngx.cpp)
    add_library(CheekyFakeStreamline MODULE tests/fake_streamline.cpp)
    add_library(CheekyFakeOpenVR MODULE tests/fake_openvr.cpp)
    foreach(fixture CheekyFakeNGX CheekyFakeStreamline CheekyFakeOpenVR)
        target_compile_features(${fixture} PRIVATE cxx_std_20)
        target_include_directories(${fixture} PRIVATE src)
        target_compile_definitions(${fixture} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(${fixture} PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures")
        if(MSVC)
            target_link_options(${fixture} PRIVATE /OPT:NOICF)
        endif()
    endforeach()
    set_target_properties(CheekyFakeNGX PROPERTIES OUTPUT_NAME "nvngx_dlss")
    set_target_properties(CheekyFakeStreamline PROPERTIES OUTPUT_NAME "sl.interposer")
    set_target_properties(CheekyFakeOpenVR PROPERTIES OUTPUT_NAME "openvr_api")
    foreach(target CheekyFoveatedDLSSRuntime CheekyFoveatedDLSSUEVR CheekyUEVRTests)
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        target_include_directories(${target} PRIVATE src shared uevr third_party/uevr/include
            third_party/openvr/include third_party/reshade/deps/minhook/include)
        target_link_libraries(${target} PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid)
        set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF PREFIX ""
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        endif()
    endforeach()
    set_target_properties(CheekyFoveatedDLSSRuntime PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/CheekyFoveatedDLSS"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/CheekyFoveatedDLSS")
    set_target_properties(CheekyFoveatedDLSSUEVR PROPERTIES OUTPUT_NAME "CheekyFoveatedDLSS"
        ARCHIVE_OUTPUT_NAME "CheekyFoveatedDLSSUEVR" PDB_NAME "CheekyFoveatedDLSSUEVR")
    add_dependencies(CheekyFoveatedDLSSUEVR CheekyFoveatedDLSSRuntime)
    add_dependencies(CheekyUEVRTests CheekyFoveatedDLSSUEVR CheekyFakeNGX CheekyFakeStreamline CheekyFakeOpenVR)
    add_test(NAME CheekyUEVRHost COMMAND CheekyUEVRTests)
    add_test(NAME CheekyUEVROwnership COMMAND CheekyUEVRTests --conflict)
    add_test(NAME CheekyUEVRDX11 COMMAND CheekyUEVRTests --dx11)
    foreach(abi 022 027 028 029)
        add_test(NAME CheekyOpenVRLate-${abi} COMMAND CheekyUEVRTests --openvr-late-${abi})
    endforeach()
    foreach(mode dx11 dx11-c dx12 dx12-c streamline streamline-dx11)
        add_test(NAME CheekyLateAttach-${mode} COMMAND CheekyUEVRTests --late-${mode})
    endforeach()
endif()
