cmake_minimum_required(VERSION 3.21)

project(DX12OverlayHook LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DX12OverlayHook is Windows-only.")
endif()

add_library(DX12OverlayHook SHARED
    src/dx12_hook.cpp
    src/cube_renderer.cpp
)

add_executable(DX12OverlayInjector
    injector/main.cpp
)

add_executable(DX12TextApp WIN32
    text_app/main.cpp
    text_app/text_bitmap.cpp
)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DX12OverlayHook and DX12OverlayInjector require an x64 build.")
endif()

target_compile_features(DX12OverlayHook PRIVATE cxx_std_17)
target_compile_definitions(DX12OverlayHook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(DX12OverlayHook PRIVATE
    d3d12
    d3dcompiler
    dxgi
)

target_compile_features(DX12OverlayInjector PRIVATE cxx_std_17)
target_compile_definitions(DX12OverlayInjector PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_features(DX12TextApp PRIVATE cxx_std_17)
target_compile_definitions(DX12TextApp PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(DX12TextApp PRIVATE
    d2d1
    d3d12
    d3dcompiler
    dwrite
    dxgi
    ole32
    windowscodecs
)
