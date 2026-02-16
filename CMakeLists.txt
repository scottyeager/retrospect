cmake_minimum_required(VERSION 3.22)
project(Retrospect VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(RETROSPECT_USE_JUCE "Build with JUCE audio backend" OFF)

# Find ncurses
find_package(Curses REQUIRED)

# Core library (pure C++ logic, no JUCE dependency)
add_library(retrospect_core STATIC
    src/core/Metronome.cpp
    src/core/RingBuffer.cpp
    src/core/Loop.cpp
    src/core/LoopEngine.cpp
)
target_include_directories(retrospect_core PUBLIC src)
target_compile_options(retrospect_core PRIVATE -Wall -Wextra -Wpedantic)

# TUI library
add_library(retrospect_tui STATIC
    src/tui/Tui.cpp
)
target_include_directories(retrospect_tui PUBLIC src ${CURSES_INCLUDE_DIRS})
target_link_libraries(retrospect_tui PUBLIC retrospect_core ${CURSES_LIBRARIES})
target_compile_options(retrospect_tui PRIVATE -Wall -Wextra -Wpedantic)

# Main executable
add_executable(retrospect src/main.cpp)
target_link_libraries(retrospect PRIVATE retrospect_core retrospect_tui)
target_compile_options(retrospect PRIVATE -Wall -Wextra -Wpedantic)

# JUCE integration (optional, for actual audio I/O)
if(RETROSPECT_USE_JUCE)
    include(FetchContent)
    FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(juce)

    juce_add_console_app(retrospect_audio
        PRODUCT_NAME "Retrospect"
    )
    target_sources(retrospect_audio PRIVATE src/audio_main.cpp)
    target_link_libraries(retrospect_audio PRIVATE
        retrospect_core
        retrospect_tui
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_core
    )
    target_compile_definitions(retrospect_audio PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
endif()
