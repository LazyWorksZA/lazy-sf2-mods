cmake_minimum_required(VERSION 3.25)
project(lazysf2 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SF2 is a 32-bit game: configure with -A Win32 (use the x86 preset).")
endif()

# Static CRT, to match the x86-windows-static vcpkg triplet and avoid runtime DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(minhook CONFIG REQUIRED)
find_package(unofficial-enet CONFIG REQUIRED)

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
    src/hooks/recorder.cpp
    src/hooks/replay.cpp
    src/net/protocol.cpp
    src/net/host_core.cpp
    src/net/enet_winmm_shim.cpp
)
set_target_properties(lazysf2 PROPERTIES OUTPUT_NAME winmm)
target_compile_features(lazysf2 PRIVATE cxx_std_20)
target_compile_definitions(lazysf2 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(lazysf2 PRIVATE /W4 /permissive-)
target_include_directories(lazysf2 PRIVATE src)
# ENet is linked statically. Do NOT link winmm here: this DLL is itself winmm.dll, and ENet's three winmm imports
# are satisfied by src/net/enet_winmm_shim.cpp (they call the real system DLL).
target_link_libraries(lazysf2 PRIVATE minhook::minhook unofficial::enet::enet ws2_32 shell32 ole32)

# Network protocol and host-core tests (console, not shipped, no game needed). ENet comes from vcpkg.
add_executable(lazysf2-net-tests
    tests/net_tests.cpp
    src/net/protocol.cpp
    src/net/host_core.cpp
)
target_compile_features(lazysf2-net-tests PRIVATE cxx_std_20)
target_compile_options(lazysf2-net-tests PRIVATE /W4 /permissive-)
target_include_directories(lazysf2-net-tests PRIVATE src)
target_link_libraries(lazysf2-net-tests PRIVATE unofficial::enet::enet ws2_32 winmm)
