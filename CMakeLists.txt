cmake_minimum_required(VERSION 3.22)
project(Retrospect VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find ncurses
find_package(Curses REQUIRED)

# Find liblo (OSC library)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBLO REQUIRED liblo)

# Fetch JUCE
include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(juce)

# Core library (pure C++ logic, no JUCE dependency)
add_library(retrospect_core STATIC
    src/core/Metronome.cpp
    src/core/MidiSync.cpp
    src/core/RingBuffer.cpp
    src/core/Loop.cpp
    src/core/LoopEngine.cpp
)
target_include_directories(retrospect_core PUBLIC src)
target_compile_options(retrospect_core PRIVATE -Wall -Wextra -Wpedantic)

# Client library (EngineClient interface + implementations)
add_library(retrospect_client STATIC
    src/client/LocalEngineClient.cpp
    src/client/OscEngineClient.cpp
)
target_include_directories(retrospect_client PUBLIC src ${LIBLO_INCLUDE_DIRS})
target_link_libraries(retrospect_client PUBLIC retrospect_core ${LIBLO_LIBRARIES})
target_link_directories(retrospect_client PUBLIC ${LIBLO_LIBRARY_DIRS})
target_compile_options(retrospect_client PRIVATE -Wall -Wextra -Wpedantic)

# Server library (OscServer)
add_library(retrospect_server STATIC
    src/server/OscServer.cpp
)
target_include_directories(retrospect_server PUBLIC src ${LIBLO_INCLUDE_DIRS})
target_link_libraries(retrospect_server PUBLIC retrospect_core ${LIBLO_LIBRARIES})
target_link_directories(retrospect_server PUBLIC ${LIBLO_LIBRARY_DIRS})
target_compile_options(retrospect_server PRIVATE -Wall -Wextra -Wpedantic)

# TUI library
add_library(retrospect_tui STATIC
    src/tui/Tui.cpp
)
target_include_directories(retrospect_tui PUBLIC src ${CURSES_INCLUDE_DIRS})
target_link_libraries(retrospect_tui PUBLIC retrospect_client ${CURSES_LIBRARIES})
target_compile_options(retrospect_tui PRIVATE -Wall -Wextra -Wpedantic)

# Main executable with JUCE audio I/O
juce_add_console_app(retrospect
    PRODUCT_NAME "Retrospect"
)
target_sources(retrospect PRIVATE src/audio_main.cpp)
target_link_libraries(retrospect PRIVATE
    retrospect_core
    retrospect_client
    retrospect_server
    retrospect_tui
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_core
)
target_compile_definitions(retrospect PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_JACK=1
    JUCE_ALSA=1
    JUCE_ALSA_MIDI_NAME="Retrospect"
    JUCE_JACK_CLIENT_NAME="Retrospect"
)
