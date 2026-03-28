
add_library(ImGui STATIC
        imconfig.h
        imgui.cpp
        imgui.h
        imgui_demo.cpp
        imgui_draw.cpp
        imgui_internal.h
        imgui_tables.cpp
        imgui_widgets.cpp
        misc/freetype/imgui_freetype.cpp
        misc/freetype/imgui_freetype.h
        backends/imgui_impl_sdl3.cpp
        backends/imgui_impl_sdl3.h
        backends/imgui_impl_sdlgpu3.cpp
        backends/imgui_impl_sdlgpu3.h
        backends/imgui_impl_sdlgpu3_shaders.h
)
target_compile_definitions(ImGui PRIVATE IMGUI_ENABLE_FREETYPE IMGUI_ENABLE_FREETYPE_LUNASVG)
target_link_libraries(ImGui PRIVATE SDL3::SDL3 freetype lunasvg)
target_include_directories(ImGui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
