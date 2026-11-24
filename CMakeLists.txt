cmake_minimum_required(VERSION 3.25)
project(lazysf2 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SF2 is a 32-bit game: configure with -A Win32 (use the x86 preset).")
endif()

# Static CRT, to match the x86-windows-static vcpkg triplet and avoid runtime DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(minhook CONFIG REQUIRED)

# The mod is loaded as a winmm.dll proxy placed next to StrikeFighters2.exe.
add_library(lazysf2 SHARED
    src/dllmain.cpp
    src/config.cpp
    src/log.cpp
    src/monitors.cpp
    src/proxy/proxy.cpp
    src/proxy/winmm.def
    src/hooks/module_watch.cpp
    src/hooks/window_hooks.cpp
    src/hooks/dxgi_hooks.cpp
    src/hooks/input_hooks.cpp
    src/hooks/menu_hooks.cpp
    src/hooks/probe_hooks.cpp
    src/hooks/puppet_hooks.cpp
    src/hooks/missile_spawn.cpp
    src/hooks/sim_hooks.cpp
)
set_target_properties(lazysf2 PROPERTIES OUTPUT_NAME winmm)
target_compile_features(lazysf2 PRIVATE cxx_std_20)
target_compile_definitions(lazysf2 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(lazysf2 PRIVATE /W4 /permissive-)
target_include_directories(lazysf2 PRIVATE src)
target_link_libraries(lazysf2 PRIVATE minhook::minhook shell32 ole32)
