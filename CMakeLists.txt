cmake_minimum_required(VERSION 3.16)
project(fc_game VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- dependencies
# X11 is required (headers + libX11/libXext are present on this system).
find_package(X11 REQUIRED)

# FreeType is optional: it enables the Chinese UI. Without it the frontend
# falls back to a built-in 8x8 bitmap font and an English interface.
find_package(Freetype QUIET)

if(NOT CMAKE_CXX_FLAGS MATCHES "-O")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()

# ---------------------------------------------------------------------- target
add_executable(fc_game
    src/core/cartridge.cpp
    src/core/mapper.cpp
    src/core/cpu.cpp
    src/core/ppu.cpp
    src/core/apu.cpp
    src/core/bus.cpp
    src/core/console.cpp
    src/state/savestate.cpp
    src/state/settings.cpp
    src/ui/platform.cpp
    src/ui/audio_out.cpp
    src/ui/text.cpp
    src/ui/menu.cpp
    src/ui/main.cpp
)

target_include_directories(fc_game PRIVATE src)

target_link_libraries(fc_game PRIVATE
    X11::X11
    X11::Xext
    ${CMAKE_DL_LIBS}          # dlopen() for the runtime-loaded ALSA
)

if(FREETYPE_FOUND)
    target_link_libraries(fc_game PRIVATE Freetype::Freetype)
    target_compile_definitions(fc_game PRIVATE HAVE_FREETYPE=1)
    message(STATUS "FreeType found - Chinese UI enabled")
else()
    message(STATUS "FreeType NOT found - falling back to built-in ASCII font")
endif()

find_package(Threads REQUIRED)
target_link_libraries(fc_game PRIVATE Threads::Threads)

# ------------------------------------------------------------------- tools
# Call tracer / coverage-aware disassembler. Development aid, not part of the
# player-facing binary.
add_executable(fctrace
    tools/fctrace.cpp
    src/core/cartridge.cpp
    src/core/mapper.cpp
    src/core/cpu.cpp
    src/core/ppu.cpp
    src/core/apu.cpp
    src/core/bus.cpp
    src/core/console.cpp
)
target_include_directories(fctrace PRIVATE src)

install(TARGETS fc_game RUNTIME DESTINATION bin)
