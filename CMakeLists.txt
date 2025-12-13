cmake_minimum_required(VERSION 3.15)

project(FitsViewer LANGUAGES C CXX)

# ====================== 基本编译选项 ======================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS 上确保生成 ARM64（Apple Silicon）
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

# ====================== 静态第三方库路径配置 ======================
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
    # ★ 按你实际生成的名字调整
    set(CFITSIO_LIB         ${CFITSIO_ROOT}/lib/cfitsio.lib)

    set(GLFW_INCLUDE_DIR    ${GLFW_ROOT}/include)
    set(GLFW_LIB            ${GLFW_ROOT}/lib/glfw3.lib)

else()
    message(FATAL_ERROR "Unsupported platform: only macOS and Windows are configured")
endif()

# 简单检查库是否存在，避免路径写错
if(NOT EXISTS ${CFITSIO_LIB})
    message(FATAL_ERROR "CFITSIO static lib not found: ${CFITSIO_LIB}")
endif()
if(NOT EXISTS ${GLFW_LIB})
    message(FATAL_ERROR "GLFW static lib not found: ${GLFW_LIB}")
endif()

# ====================== 系统依赖：OpenGL / zlib ======================
find_package(OpenGL REQUIRED)

if(APPLE)
    # 如果你的 cfitsio 是带 zlib 的，必须连 zlib
    find_package(ZLIB REQUIRED)
endif()

# ====================== 第三方源码：ImGui / glad ======================
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

# ====================== 头文件搜索路径（全局） ======================
# 简单起见用 include_directories，你也可以改成 target_include_directories
include_directories(
        ${CMAKE_SOURCE_DIR}/include               # kty/FitsRenderer.h, StackCore.h
        ${CMAKE_SOURCE_DIR}/src/core             # ★ 确保能找到 FitsImage.h / Debayer.h / Stretch.h
        ${CMAKE_SOURCE_DIR}/external             # stb_image_write.h
        ${CMAKE_SOURCE_DIR}/third_party_gl/include
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMGUI_DIR}/misc/cpp
        ${CFITSIO_INCLUDE_DIR}
        ${GLFW_INCLUDE_DIR}
)

# ====================== 核心库：kty_fits_core ======================
add_library(kty_fits_core
        src/core/FitsRenderer.cpp
        src/core/GlImageRenderer.cpp
        src/core/FitsImage.cpp
        src/core/Debayer.cpp
        src/core/Stretch.cpp
)

target_link_libraries(kty_fits_core
        PRIVATE
        ${CFITSIO_LIB}
        OpenGL::GL
)

if(APPLE)
    target_link_libraries(kty_fits_core
            PRIVATE
            ZLIB::ZLIB
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
elseif(WIN32)
    target_link_libraries(kty_fits_core
            PRIVATE
            gdi32
            user32
            shell32
            advapi32
            winmm
    )
endif()

# ====================== 深空叠加核心库：kty_stack_core ======================
add_library(kty_stack_core
        src/stack/StackCore.cpp
)

target_link_libraries(kty_stack_core
        PRIVATE
        kty_fits_core
)

# ====================== ImGui 前端可执行文件 ======================
add_executable(FitsViewer_imgui
        src/main.cpp
        src/ui/ImguiApp.cpp
        ${IMGUI_SOURCES}
        ${GLAD_SOURCES}
)

# 告诉 ImGui backend 我们用的是 GLAD 作为 loader
target_compile_definitions(FitsViewer_imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)

target_link_libraries(FitsViewer_imgui
        PRIVATE
        kty_fits_core
        kty_stack_core
        ${GLFW_LIB}
        OpenGL::GL
)

if(APPLE)
    target_link_libraries(FitsViewer_imgui
            PRIVATE
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
    )
elseif(WIN32)
    target_link_libraries(FitsViewer_imgui
            PRIVATE
            gdi32
            user32
            shell32
            advapi32
            winmm
    )
endif()
