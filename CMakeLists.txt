cmake_minimum_required(VERSION 3.15)
project(FitsViewer LANGUAGES C CXX)

# ---------------- C++ 标准 ----------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------- CFITSIO 手动查找 ----------------
# 如用 Homebrew: brew install cfitsio
set(CFITSIO_HINT_PATHS
    /opt/homebrew        # Apple Silicon 默认
    /usr/local           # Intel Mac / 一些 Linux
    /usr
)

find_path(CFITSIO_INCLUDE_DIR
    NAMES fitsio.h
    PATH_SUFFIXES include
    PATHS ${CFITSIO_HINT_PATHS}
)

find_library(CFITSIO_LIBRARY
    NAMES cfitsio
    PATH_SUFFIXES lib
    PATHS ${CFITSIO_HINT_PATHS}
)

if (NOT CFITSIO_INCLUDE_DIR OR NOT CFITSIO_LIBRARY)
    message(FATAL_ERROR "CFITSIO not found. Please install it first (e.g. brew install cfitsio).")
else()
    message(STATUS "CFITSIO include: ${CFITSIO_INCLUDE_DIR}")
    message(STATUS "CFITSIO library: ${CFITSIO_LIBRARY}")
endif()

# ---------------- OpenGL & GLFW ----------------
# Homebrew: brew install glfw
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# ---------------- ImGui 路径 ----------------
# 请把官方 imgui 仓库放到 third_party/imgui
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# ---------------- GLAD ----------------
# 在线生成的 glad 放在 third_party_gl
set(GLAD_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party_gl/include)
set(GLAD_SOURCES      ${CMAKE_SOURCE_DIR}/third_party_gl/src/glad.c)

# ---------------- 头文件搜索路径 ----------------
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CFITSIO_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
    ${GLAD_INCLUDE_DIR}
)

# ---------------- ImGui 源码列表 ----------------
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

# ---------------- 可执行文件 ----------------
add_executable(FitsViewer
    src/main.cpp
    src/FitsImage.cpp
    src/Debayer.cpp
    src/Stretch.cpp
    src/ImageApp.cpp
    src/GlImageRenderer.cpp
    ${IMGUI_SOURCES}
    ${GLAD_SOURCES}
)

# 让 imgui_impl_opengl3 使用 GLAD 作为 OpenGL loader
target_compile_definitions(FitsViewer
    PRIVATE
        IMGUI_IMPL_OPENGL_LOADER_GLAD
)

# ---------------- 链接库 ----------------
target_link_libraries(FitsViewer
    PRIVATE
        ${CFITSIO_LIBRARY}
        glfw
        OpenGL::GL
)

# ---------------- macOS 额外 Framework ----------------
if (APPLE)
    target_link_libraries(FitsViewer
        PRIVATE
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
endif()

# ---------------- Windows 上去掉控制台窗口（可选） ----------------
if (WIN32)
    set_target_properties(FitsViewer PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()
