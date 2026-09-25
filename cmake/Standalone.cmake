if(NOT MSVC)
    message(FATAL_ERROR "Standalone DXGI forwarding requires MSVC x64 MASM")
endif()
enable_language(ASM_MASM)
set(CHEEKY_IMGUI third_party/reshade/deps/imgui)
add_library(CheekyFoveatedDLSSHost MODULE
    standalone/host.cpp standalone/vulkan_overlay.cpp src/vulkan_api.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_vulkan.cpp standalone/overlay.cpp standalone/overlay_input.cpp standalone/overlay_ui.cpp uevr/settings_io.cpp src/settings.cpp src/foveation.cpp
    ${CHEEKY_IMGUI}/imgui.cpp ${CHEEKY_IMGUI}/imgui_draw.cpp
    ${CHEEKY_IMGUI}/imgui_tables.cpp ${CHEEKY_IMGUI}/imgui_widgets.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_win32.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_dx11.cpp ${CHEEKY_IMGUI}/backends/imgui_impl_dx12.cpp
    third_party/reshade/deps/minhook/src/buffer.c
    third_party/reshade/deps/minhook/src/hook.c
    third_party/reshade/deps/minhook/src/trampoline.c
    third_party/reshade/deps/minhook/src/hde/hde64.c)
target_compile_definitions(CheekyFoveatedDLSSHost PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_include_directories(CheekyFoveatedDLSSHost PRIVATE third_party/vulkan/include standalone shared src uevr
    ${CHEEKY_IMGUI} ${CHEEKY_IMGUI}/backends third_party/reshade/deps/minhook/include)
target_link_libraries(CheekyFoveatedDLSSHost PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid user32 imm32 dwmapi)
add_dependencies(CheekyFoveatedDLSSHost CheekyFoveatedDLSSRuntime)
add_library(CheekyStandaloneProxy MODULE bootstrap/loader.cpp bootstrap/dxgi_proxy.cpp
    bootstrap/dxgi_exports.asm bootstrap/dxgi.def)
add_library(CheekyOptiScaler MODULE bootstrap/loader.cpp bootstrap/optiscaler_asi.cpp)
add_executable(CheekyOverlayTests tests/overlay_tests.cpp standalone/overlay.cpp standalone/overlay_input.cpp standalone/overlay_ui.cpp
    uevr/settings_io.cpp src/settings.cpp src/foveation.cpp
    ${CHEEKY_IMGUI}/imgui.cpp ${CHEEKY_IMGUI}/imgui_draw.cpp
    ${CHEEKY_IMGUI}/imgui_tables.cpp ${CHEEKY_IMGUI}/imgui_widgets.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_win32.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_dx11.cpp ${CHEEKY_IMGUI}/backends/imgui_impl_dx12.cpp)
target_sources(CheekyOverlayTests PRIVATE
    tests/vulkan_overlay_tests.cpp standalone/vulkan_overlay.cpp src/vulkan_api.cpp
    ${CHEEKY_IMGUI}/backends/imgui_impl_vulkan.cpp
    third_party/reshade/deps/minhook/src/buffer.c
    third_party/reshade/deps/minhook/src/hook.c
    third_party/reshade/deps/minhook/src/trampoline.c
    third_party/reshade/deps/minhook/src/hde/hde64.c)
target_include_directories(CheekyOverlayTests PRIVATE standalone shared src uevr ${CHEEKY_IMGUI}
    third_party/reshade/deps/minhook/include third_party/vulkan/include)
target_link_libraries(CheekyOverlayTests PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid user32 imm32 dwmapi)
target_compile_definitions(CheekyOverlayTests PRIVATE CHEEKY_OVERLAY_TEST_DESKTOP IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
add_test(NAME CheekyOverlay-UI COMMAND CheekyOverlayTests --ui)
foreach(target CheekyFoveatedDLSSHost CheekyStandaloneProxy CheekyOptiScaler CheekyOverlayTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
endforeach()
foreach(api dx11 dx12)
    foreach(color sdr hdr10 scrgb)
        add_test(NAME CheekyOverlay-${api}-${color} COMMAND CheekyOverlayTests --${api} --${color})
        set_tests_properties(CheekyOverlay-${api}-${color} PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    endforeach()
endforeach()
set_target_properties(CheekyFoveatedDLSSHost PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/CheekyFoveatedDLSS"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/CheekyFoveatedDLSS")
# Keep proxy DLLs away from test executables: otherwise the Windows loader
# would inject Cheeky into every native regression test before main().
set_target_properties(CheekyStandaloneProxy PROPERTIES OUTPUT_NAME dxgi
    ARCHIVE_OUTPUT_NAME CheekyStandaloneProxy PDB_NAME CheekyStandaloneProxy
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/standalone-loader"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/standalone-loader")
set_target_properties(CheekyOptiScaler PROPERTIES OUTPUT_NAME CheekyFoveatedDLSS SUFFIX .asi
    ARCHIVE_OUTPUT_NAME CheekyOptiScaler PDB_NAME CheekyOptiScaler)

add_executable(CheekyRuntimeHostTests tests/runtime_host_tests.cpp)
target_include_directories(CheekyRuntimeHostTests PRIVATE src shared uevr)
target_link_libraries(CheekyRuntimeHostTests PRIVATE d3d11 d3d12 dxgi)
add_dependencies(CheekyRuntimeHostTests CheekyFoveatedDLSSRuntime)
add_executable(CheekyBootstrapTests bootstrap/tests.cpp)
add_library(CheekyBootstrapFakeHost MODULE bootstrap/fake_host.cpp)
add_library(CheekyFakeCore MODULE tests/fake_ngx_core.cpp tests/fake_ngx_core.rc)
add_library(CheekyFakeCoreProxy MODULE tests/fake_ngx_core.cpp tests/fake_ngx_core.rc)
target_compile_definitions(CheekyFakeCoreProxy PRIVATE CHEEKY_FAKE_CORE_PROXY)
add_executable(CheekyCoreDiscoveryTests tests/core_discovery_tests.cpp)
target_include_directories(CheekyCoreDiscoveryTests PRIVATE shared src tests)
foreach(target CheekyRuntimeHostTests CheekyBootstrapTests CheekyBootstrapFakeHost CheekyFakeCore CheekyFakeCoreProxy CheekyCoreDiscoveryTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
endforeach()
foreach(target CheekyFakeCore CheekyFakeCoreProxy)
    target_include_directories(${target} PRIVATE src)
    target_link_options(${target} PRIVATE /OPT:NOICF)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures")
endforeach()
add_dependencies(CheekyCoreDiscoveryTests CheekyFoveatedDLSSRuntime CheekyFakeCore CheekyFakeCoreProxy)
foreach(mode accept reject)
    add_test(NAME CheekyCoreDiscovery-${mode} COMMAND CheekyCoreDiscoveryTests ${mode}
        $<TARGET_FILE:CheekyFoveatedDLSSRuntime> $<TARGET_FILE:CheekyFakeCore> $<TARGET_FILE:CheekyFakeCoreProxy>)
endforeach()
if(TARGET CheekyFakeNGX)
    add_dependencies(CheekyCoreDiscoveryTests CheekyFakeNGX)
    add_test(NAME CheekyD3D11OtaDiscovery COMMAND CheekyCoreDiscoveryTests dx11-ota
        $<TARGET_FILE:CheekyFoveatedDLSSRuntime> $<TARGET_FILE:CheekyFakeNGX> unused)
endif()
add_dependencies(CheekyBootstrapTests CheekyStandaloneProxy CheekyOptiScaler CheekyBootstrapFakeHost)
foreach(mode proxy asi missing)
    add_test(NAME CheekyBootstrap-${mode} COMMAND CheekyBootstrapTests ${mode}
        $<TARGET_FILE:CheekyStandaloneProxy> $<TARGET_FILE:CheekyOptiScaler> $<TARGET_FILE:CheekyBootstrapFakeHost>)
endforeach()
add_test(NAME CheekyRuntimeStandalone-DX12 COMMAND CheekyRuntimeHostTests)
add_test(NAME CheekyRuntimeStandalone-DX11 COMMAND CheekyRuntimeHostTests --dx11)
add_test(NAME CheekyRuntimeOptiScaler-DX12 COMMAND CheekyRuntimeHostTests --optiscaler)
add_test(NAME CheekyRuntimeOptiScaler-DX11 COMMAND CheekyRuntimeHostTests --optiscaler --dx11)
add_test(NAME CheekyRuntimeOwnership COMMAND CheekyRuntimeHostTests --conflict)

if(CHEEKY_BUILD_UEVR)
    add_test(NAME CheekyRuntimeTransport-Ota COMMAND CheekyRuntimeHostTests --transport-ota)
    add_test(NAME CheekyRuntimeTransport-OtaOnly COMMAND CheekyRuntimeHostTests --transport-ota-only)
    add_dependencies(CheekyRuntimeHostTests CheekyFakeNGX CheekyFakeOpenVR)
    add_test(NAME CheekyRuntimeStandalone-Transport COMMAND CheekyRuntimeHostTests --transport)
    add_test(NAME CheekyRuntimeStandalone-TransportInitFailure COMMAND CheekyRuntimeHostTests --transport-init-failure)
    add_test(NAME CheekyRuntimeOptiScaler-Transport COMMAND CheekyRuntimeHostTests --optiscaler --transport)
    foreach(abi 022 027 028 029)
        add_test(NAME CheekyRuntimeStandalone-OpenVR-${abi} COMMAND CheekyRuntimeHostTests --openvr-late-${abi})
        add_test(NAME CheekyRuntimeOptiScaler-OpenVR-${abi} COMMAND CheekyRuntimeHostTests --optiscaler --openvr-late-${abi})
    endforeach()
    add_executable(CheekyStandaloneHostTests tests/standalone_host_tests.cpp tests/late_attach_tests.cpp)
    target_compile_features(CheekyStandaloneHostTests PRIVATE cxx_std_20)
    target_compile_definitions(CheekyStandaloneHostTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(CheekyStandaloneHostTests PRIVATE standalone src shared uevr)
    target_link_libraries(CheekyStandaloneHostTests PRIVATE d3d11 d3d12 dxgi d3dcompiler dxguid)
    set_target_properties(CheekyStandaloneHostTests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_dependencies(CheekyStandaloneHostTests CheekyFoveatedDLSSHost CheekyFakeNGX CheekyFakeStreamline CheekyFakeRealVR)
    foreach(mode native native-c streamline missing-lower public-first public-first-c ota ota-c ota-streamline ota-ambiguous dx12-rr dx12-rr-c dx12-rr-ota dx12-rr-streamline dx12-rr-streamline-c dx12-rr-ota-streamline)
        add_test(NAME CheekyStandaloneRealVR-${mode} COMMAND CheekyStandaloneHostTests realvr-${mode} standalone)
    endforeach()
    foreach(host standalone optiscaler)
        foreach(mode dx11 dx11-c dx12 dx12-c streamline streamline-dx11)
            add_test(NAME CheekyHost-${host}-${mode} COMMAND CheekyStandaloneHostTests ${mode} ${host})
        endforeach()
    endforeach()
endif()

add_library(CheekyVulkanLayer MODULE vulkan_layer/bootstrap.cpp)
target_compile_features(CheekyVulkanLayer PRIVATE cxx_std_20)
target_compile_definitions(CheekyVulkanLayer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(CheekyVulkanLayer PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
