cmake_minimum_required(VERSION 3.15)

project(FitsViewer LANGUAGES C CXX)

# ====================== 基本编译选项 ======================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS 上确保生成 arm64（Apple Silicon）
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

# ====================== 平台相关静态库路径 ======================
if(APPLE)
    message(STATUS "Configuring for macOS (ARM64)")

    set(PLATFORM_STATIC_ROOT ${CMAKE_SOURCE_DIR}/third_party_static/macos)

    set(CFITSIO_ROOT ${PLATFORM_STATIC_ROOT}/cfitsio)
    set(GLFW_ROOT    ${PLATFORM_STATIC_ROOT}/glfw)

    set(CFITSIO_INCLUDE_DIR ${CFITSIO_ROOT}/include)
    set(CFITSIO_LIB         ${CFITSIO_ROOT}/lib/libcfitsio.a)

    set(GLFW_INCLUDE_DIR    ${GLFW_ROOT}/include)
    set(GLFW_LIB            ${GLFW_ROOT}/lib/libglfw3.a)

elseif(WIN32)
    message(STATUS "Configuring for Windows (x64)")

    set(PLATFORM_STATIC_ROOT ${CMAKE_SOURCE_DIR}/third_party_static/windows)

    set(CFITSIO_ROOT ${PLATFORM_STATIC_ROOT}/cfitsio)
    set(GLFW_ROOT    ${PLATFORM_STATIC_ROOT}/glfw)

    set(CFITSIO_INCLUDE_DIR ${CFITSIO_ROOT}/include)
    # ★ 按你实际编译出的名字改
    set(CFITSIO_LIB         ${CFITSIO_ROOT}/lib/cfitsio.lib)

    set(GLFW_INCLUDE_DIR    ${GLFW_ROOT}/include)
    set(GLFW_LIB            ${GLFW_ROOT}/lib/glfw3.lib)

else()
    message(FATAL_ERROR "Unsupported platform: only macOS and Windows are configured")
endif()

# 简单检查库是否存在，防止路径写错
if(NOT EXISTS ${CFITSIO_LIB})
    message(FATAL_ERROR "CFITSIO static lib not found: ${CFITSIO_LIB}")
endif()
if(NOT EXISTS ${GLFW_LIB})
    message(FATAL_ERROR "GLFW static lib not found: ${GLFW_LIB}")
endif()

# ====================== 查找 OpenGL / zlib ======================
find_package(OpenGL REQUIRED)

if(APPLE)
    # cfitsio 默认带 zlib 压缩支持（如果你在 configure 里没关掉）
    find_package(ZLIB REQUIRED)
endif()

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
    src/EmbeddedFont.cpp        # 如果没有内嵌字体，这行可以删掉
    ${IMGUI_SOURCES}
    ${GLAD_SOURCES}
)

target_compile_definitions(FitsViewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)

# ====================== 链接静态库 + 系统库 ======================
if(APPLE)
    target_link_libraries(FitsViewer
        PRIVATE
            ${CFITSIO_LIB}
            ${GLFW_LIB}
            ZLIB::ZLIB          # 保留 zlib 压缩支持（如果 configure 时没关）
            OpenGL::GL
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )

elseif(WIN32)
    target_link_libraries(FitsViewer
        PRIVATE
            ${CFITSIO_LIB}
            ${GLFW_LIB}
            OpenGL::GL          # 通常映射到 opengl32.lib
            gdi32
            user32
            shell32
            advapi32
            winmm
    )
endif()
