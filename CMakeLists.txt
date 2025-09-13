cmake_minimum_required(VERSION 3.16)
project(Haywire VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find packages
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Platform-specific settings
if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(CORE_VIDEO CoreVideo)
    set(PLATFORM_LIBS ${COCOA_LIBRARY} ${IOKIT_LIBRARY} ${CORE_VIDEO})
elseif(UNIX)
    find_package(X11 REQUIRED)
    set(PLATFORM_LIBS ${X11_LIBRARIES} dl)
elseif(WIN32)
    set(PLATFORM_LIBS)
endif()

# GLFW
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
    )
    FetchContent_MakeAvailable(glfw)
endif()

# ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Capstone (disabled for now - not needed yet)
find_package(PkgConfig)
# if(PKG_CONFIG_FOUND)
#     pkg_check_modules(CAPSTONE capstone)
# endif()
# 
# # Fallback for Homebrew on Apple Silicon
# if(NOT CAPSTONE_FOUND AND APPLE)
#     find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h
#         PATHS /opt/homebrew/include /usr/local/include)
#     find_library(CAPSTONE_LIBRARY capstone
#         PATHS /opt/homebrew/lib /usr/local/lib)
#     if(CAPSTONE_INCLUDE_DIR AND CAPSTONE_LIBRARY)
#         set(CAPSTONE_FOUND TRUE)
#         set(CAPSTONE_INCLUDE_DIRS ${CAPSTONE_INCLUDE_DIR})
#         set(CAPSTONE_LIBRARIES ${CAPSTONE_LIBRARY})
#     endif()
# endif()
set(CAPSTONE_FOUND FALSE)

# JSON library (using nlohmann/json)
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)

# Main executable
add_executable(haywire
    src/main.cpp
    src/qemu_connection.cpp
    src/gdb_connection.cpp
    src/mmap_reader.cpp
    src/memory_backend.cpp
    src/memory_mapper.cpp
    src/memory_utils.cpp
    src/guest_agent.cpp
    src/viewport_translator.cpp
    src/process_memory_map.cpp
    src/address_space_flattener.cpp
    src/pagemap_cache.cpp
    src/crunched_memory_reader.cpp
    src/memory_visualizer.cpp
    src/memory_renderer.cpp
    src/memory_overview.cpp
    src/hex_overlay.cpp
    src/autocorrelator.cpp
    src/fft16384.cpp
    src/beacon_reader.cpp
    src/beacon_decoder.cpp
    src/beacon_translator.cpp
    src/pid_selector.cpp
    src/bitmap_viewer.cpp
    src/address_parser.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(haywire PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${OPENGL_INCLUDE_DIRS}
)

target_link_libraries(haywire PRIVATE
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${PLATFORM_LIBS}
)

if(CAPSTONE_FOUND)
    target_compile_definitions(haywire PRIVATE HAS_CAPSTONE)
    target_include_directories(haywire PRIVATE ${CAPSTONE_INCLUDE_DIRS})
    if(APPLE AND CAPSTONE_LIBRARY)
        # Use full path on macOS
        target_link_libraries(haywire PRIVATE ${CAPSTONE_LIBRARY})
    else()
        target_link_libraries(haywire PRIVATE ${CAPSTONE_LIBRARIES})
    endif()
endif()

# Compile definitions for ImGui
if(APPLE)
    target_compile_definitions(haywire PRIVATE
        GL_SILENCE_DEPRECATION
    )
endif()

# Copy assets to build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# Test executables moved to test_attempts/ directory