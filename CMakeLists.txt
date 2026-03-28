cmake_minimum_required(VERSION 3.22)

project(omplayer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif ()

add_subdirectory(third_party/external/freetype)
add_subdirectory(third_party/external/lunasvg)
set(AUI_BOOT OFF)
set(OPENMEDIA_INSTALL OFF)
add_subdirectory(third_party/external/OpenMedia)
add_subdirectory(third_party/external/SDL3)
add_subdirectory(third_party/imgui)

add_executable(omplayer WIN32
        src/audio_sink.hpp
        src/av_clock.hpp
        src/frame_queue.hpp
        src/main.cpp
        src/media_player.hpp
        src/ring_buffer.hpp
        src/ui.hpp
        src/video_renderer.hpp
)
target_link_libraries(omplayer PRIVATE SDL3::SDL3 OpenMedia::OpenMedia ImGui)
