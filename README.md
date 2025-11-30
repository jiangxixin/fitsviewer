

# 空天望远镜 FITS Viewer

一个针对天文图像（尤其是行星 / 深空）设计的 **FITS 图像浏览与预处理工具**：

* 全流程 GPU 处理：去拜耳、白平衡、拉伸、缩放、直方图、导出
* 支持多种 Bayer 模式和类似 NINA 的拉伸方式
* PNG 导出与预览效果完全一致
* 跨平台：macOS (Apple Silicon) / Windows (x64)，静态链接 cfitsio + glfw3

> 当前版本仅支持本地 FITS 文件，不包含 HTTP/HTTPS/S3 等网络读取功能（编译时关闭了 curl/SSL）。

---

## ✨ 功能特性

### FITS 加载

* 基于 **CFITSIO** 读取 FITS 图像
* 支持单通道 Bayer RAW / 灰度数据
* 当前默认读取主图像扩展（HDU0）

### GPU 去拜耳 + 渲染管线

* 使用 OpenGL + GLSL，在 GPU 上完成：

  * Bayer 去拜耳（全分辨率双线性插值）
  * 白平衡（R/G/B 增益）
  * 自动拉伸（Auto Stretch）
  * 手动 Tone Curve
  * 视图缩放 / 平移
* 支持 Bayer 模式：

  * `None`
  * `RGGB`
  * `BGGR`
  * `GRBG`
  * `GBRG`

### 多种拉伸模式

* 自动拉伸参数通过 GPU 统计得到：

  * 在 256×256 的统计纹理上渲染亮度
  * CPU 只对 65k 点做 percentile 统计
* 拉伸模式：

  * **Linear**
  * **Arcsinh**
  * **Log**
  * **Sqrt**
* UI 可调参数：

  * `Black clip %` / `White clip %`（0–20%）
  * `Stretch strength`（控制 Asinh / Log 的曲线强度）
  * `Auto Stretch` 开关

### 实时亮度直方图

* 基于 **拉伸后的亮度** 计算直方图（与当前画面一致）
* 使用 64 个 bin，经过归一化和 `sqrt` 视觉增强，使暗部结构更明显
* 直方图会随着以下变化实时更新：

  * Black/White clip
  * 拉伸模式 / 强度
  * Bayer 模式
  * 白平衡
  * 重新加载 FITS 文件

### 白平衡 & 视图控制

* 白平衡：

  * `R gain / G gain / B gain` 三通道独立调节
  * 直接在 shader 中应用，预览和直方图同时更新
* 视图：

  * `Scale` 滑块（0.1x–20x，对数滑块）
  * 右键拖动平移图像
  * `Reset View` 按钮恢复默认视图

### PNG 导出（与预览一致）

* 使用同一个 shader + 当前所有参数，在离屏 FBO 渲染全分辨率图像
* 通过 `glReadPixels` 读回 RGB8，再用 `stb_image_write` 写 PNG
* 导出文件名：

  * 基于当前 FITS 文件名自动替换扩展名为 `.png`
  * 例如 `M42.fits` → `M42.png`
* 导出成功后：

  * 控制面板显示绿色提示：`导出成功: <输出路径>`

### ImGui UI & 中文支持

* 使用 Dear ImGui + `imgui_impl_glfw` + `imgui_impl_opengl3`
* 字体：

  * 使用 ImGui 自带的 `binary_to_compressed_c.cpp` 将中文字体压缩为 C 数组
  * 通过 `AddFontFromMemoryCompressedTTF` 从内存加载
  * macOS / Windows 统一字体，中文 UI 不乱码
* 基于 ImGui 的控制面板，图像作为背景铺满窗口

### 静态链接 & 平台支持

* **macOS (Apple Silicon)**：

  * 自行编译静态 `libcfitsio.a` / `libglfw3.a`
  * 动态依赖仅包括系统 Framework：`Cocoa / IOKit / CoreVideo / OpenGL`
* **Windows (x64)**：

  * 自行编译 `cfitsio.lib` / `glfw3.lib` 静态库
  * ImGui / glad / stb 使用源代码随工程编译

---

## 🧱 依赖

### 公共依赖（源码内集成）

* [Dear ImGui](https://github.com/ocornut/imgui)
* [glad](https://github.com/Dav1dde/glad)
* [stb_image_write.h](https://github.com/nothings/stb)
* OpenGL 3.3+（桌面 GL）

### macOS

* CFITSIO（静态编译：`libcfitsio.a`）
* GLFW3（静态编译：`libglfw3.a`）
* 系统 Framework：

  * Cocoa
  * IOKit
  * CoreVideo
  * OpenGL

### Windows

* CFITSIO（静态编译：`cfitsio.lib`）
* GLFW3（静态编译：`glfw3.lib`）
* 系统库：

  * opengl32
  * gdi32
  * user32
  * shell32
  * advapi32
  * winmm

---

## 📁 目录结构建议

```text
your_project/
  CMakeLists.txt
  src/
    main.cpp
    FitsImage.cpp / .h
    Debayer.cpp / .h
    Stretch.cpp / .h
    ImageApp.cpp / .h
    GlImageRenderer.cpp / .h
    EmbeddedFont.cpp / .h
  third_party/
    imgui/
      imgui.cpp / .h ...
      backends/imgui_impl_glfw.cpp / .h
      backends/imgui_impl_opengl3.cpp / .h
      misc/cpp/imgui_stdlib.cpp / .h
      misc/fonts/binary_to_compressed_c.cpp (工具，只在生成字体时用)
  third_party_gl/
    include/
      glad/glad.h
      KHR/khrplatform.h
    src/
      glad.c
  external/
    stb_image_write.h
  third_party_static/
    macos/
      cfitsio/
        include/
        lib/libcfitsio.a
      glfw/
        include/
        lib/libglfw3.a
    windows/
      cfitsio/
        include/
        lib/cfitsio.lib
      glfw/
        include/
        lib/glfw3.lib
```

---

## 🧩 编译方式

### macOS (Apple Silicon)

1. 编译 CFITSIO 静态库（禁用 curl/SSL，仅保留本地文件和 zlib 压缩）：

```bash
cd /tmp
tar xf cfitsio-*.tar.gz
cd cfitsio-*/

./configure \
  --disable-shared \
  --enable-static \
  --disable-curl \
  --prefix=/path/to/your_project/third_party_static/macos/cfitsio \
  CFLAGS="-O3 -arch arm64" \
  LDFLAGS="-arch arm64"

make -j8
make install
```

2. 编译 GLFW3 静态库：

```bash
cd /tmp
git clone https://github.com/glfw/glfw.git
cd glfw
git checkout 3.4  # 或其他稳定版本

mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DGLFW_BUILD_EXAMPLES=OFF \
  -DGLFW_BUILD_TESTS=OFF \
  -DGLFW_BUILD_DOCS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_INSTALL_PREFIX=/path/to/your_project/third_party_static/macos/glfw

cmake --build . --config Release --target install -j8
```

3. 编译本项目：

```bash
cd /path/to/your_project
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
```

---

### Windows (x64)

1. 用 CMake / VS 构建静态 `cfitsio.lib`：

   * 在 cfitsio 源码目录用 CMake 或 CMake+NMake / Ninja 生成静态库
   * 安装到 `third_party_static/windows/cfitsio/{include,lib}`

2. 用 CMake 构建 GLFW3 静态库 `glfw3.lib`：

```pwsh
cd C:\path\to\glfw
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF -A x64 -DCMAKE_INSTALL_PREFIX=C:/path/to/your_project/third_party_static/windows/glfw
cmake --build . --config Release --target INSTALL
```

3. 编译本项目：

```pwsh
cd C:\path\to\your_project
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

---

## 🖱 操作说明

* **打开 FITS**

  * 在 `FITS Path` 输入路径或点击 `Browse...` 选择文件
  * 点击 `Load FITS` 载入图像

* **视图操作**

  * `Scale` 滑块缩放图像
  * 按住鼠标右键拖动平移图像
  * `Reset View` 恢复默认视图范围

* **Bayer & 白平衡**

  * `Bayer` 下拉选择合适的 Bayer 模式（常见天文相机为 RGGB 或 BGGR）
  * 调整 `R/G/B gain` 做简单白平衡

* **拉伸 & 直方图**

  * 勾选 `Auto Stretch`，调整 `Black clip % / White clip % / Stretch strength`
  * 直方图显示当前拉伸后的亮度分布，便于观察黑白点位置和动态范围

* **导出 PNG**

  * 点击 `Export PNG`
  * 导出文件会与当前 FITS 同名（扩展名改为 `.png`）
  * 控制面板会显示导出成功提示和完整路径

---

## 🔮 未来计划

* 支持多 HDU / 多图层浏览
* 增强直方图交互（在直方图上直接拖动黑白点）
* 更完整的颜色管理 / 色彩空间（例如 sRGB ↔ 线性空间转换）
* 实验性 WebAssembly 版（Emscripten + WebGL2）

---

欢迎提 issue 交流使用体验或改进建议 🤝
