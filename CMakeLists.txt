cmake_minimum_required(VERSION 3.15)

project(FitsViewer LANGUAGES C CXX)

# ====================== 基本编译选项 ======================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 确保生成 arm64 可执行文件（Apple Silicon）
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

# ====================== 第三方静态库路径 ======================
# 手动编译好的 cfitsio / glfw3 静态库安装位置
set(CFITSIO_ROOT ${CMAKE_SOURCE_DIR}/third_party_static/cfitsio)
set(GLFW_ROOT    ${CMAKE_SOURCE_DIR}/third_party_static/glfw)

set(CFITSIO_INCLUDE_DIR ${CFITSIO_ROOT}/include)
set(CFITSIO_LIB         ${CFITSIO_ROOT}/lib/libcfitsio.a)

set(GLFW_INCLUDE_DIR    ${GLFW_ROOT}/include)
set(GLFW_LIB            ${GLFW_ROOT}/lib/libglfw3.a)

# 简单检查库是否存在，防止路径写错
if(NOT EXISTS ${CFITSIO_LIB})
    message(FATAL_ERROR "CFITSIO static lib not found: ${CFITSIO_LIB}")
endif()
if(NOT EXISTS ${GLFW_LIB})
    message(FATAL_ERROR "GLFW static lib not found: ${GLFW_LIB}")
endif()

# ====================== OpenGL & 系统框架 ======================
find_package(OpenGL REQUIRED)

find_package(ZLIB REQUIRED)

# ====================== ImGui / glad 源码 ======================
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

set(GLAD_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party_gl/src/glad.c
)

# ====================== 头文件搜索路径 ======================
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external              # stb_image_write.h 等
    ${CMAKE_SOURCE_DIR}/third_party_gl/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
    ${CFITSIO_INCLUDE_DIR}
    ${GLFW_INCLUDE_DIR}
)

# ====================== 可执行文件 ======================
add_executable(FitsViewer
    src/main.cpp
    src/FitsImage.cpp
    src/Debayer.cpp
    src/Stretch.cpp
    src/ImageApp.cpp
    src/GlImageRenderer.cpp
    src/EmbeddedFont.cpp        # 如果没用内嵌字体，可以删掉这一行
    ${IMGUI_SOURCES}
    ${GLAD_SOURCES}
)

# ImGui OpenGL backend 使用 GLAD 作为 loader
target_compile_definitions(FitsViewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)

# ====================== 链接静态库和系统库 ======================
target_link_libraries(FitsViewer
    PRIVATE
        ${CFITSIO_LIB}
        ${GLFW_LIB}
        ZLIB::ZLIB
        OpenGL::GL
)

# macOS 下需要显式链接系统 Framework
if(APPLE)
    target_link_libraries(FitsViewer
        PRIVATE
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
endif()
