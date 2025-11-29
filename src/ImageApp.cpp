#include "ImageApp.h"
#include "Debayer.h"
#include "Stretch.h"   // 用 tone_curve 画曲线 & 导出
#include "FitsImage.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <iostream>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

ImageApp::ImageApp() {}
ImageApp::~ImageApp()
{
    shutdown();
}

bool ImageApp::init()
{
    if (!glfwInit())
    {
        std::cerr << "Failed to init GLFW\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    _window = glfwCreateWindow(1280, 720, "FITS Viewer", nullptr, nullptr);
    if (!_window)
    {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(_window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to init GLAD\n";
        return false;
    }

    if (!_renderer.init())
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(_window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    try
    {
        _fileDialogDir = fs::current_path().string();
    }
    catch (...)
    {
        _fileDialogDir = ".";
    }
    _fileListDirty = true;

    return true;
}

void ImageApp::shutdown()
{
    _renderer.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (_window)
    {
        glfwDestroyWindow(_window);
        _window = nullptr;
    }
    glfwTerminate();
}

void ImageApp::run()
{
    if (!init())
        return;

    main_loop();
    shutdown();
}

void ImageApp::main_loop()
{
    while (!glfwWindowShouldClose(_window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        render_ui();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(_window, &display_w, &display_h);

        // ==== 鼠标缩放 / 平移 ====
        ImGuiIO& io = ImGui::GetIO();

        if (_hasImage)
        {
            // 滚轮缩放（macOS 上用两指上下滑动）
            if (io.MouseWheel != 0.0f)
            {
                float zoomFactor = 1.0f + io.MouseWheel * 0.1f; // 每格 ~10%
                _zoom *= zoomFactor;
                if (_zoom < 0.1f) _zoom = 0.1f;
                if (_zoom > 20.0f) _zoom = 20.0f;
            }

            // 右键拖动平移
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                float dx = -io.MouseDelta.x / (float)display_w;
                float dy =  io.MouseDelta.y / (float)display_h;
                _panX += dx * _zoom;
                _panY += dy * _zoom;
            }
        }

        // 传给 GPU
        _renderer.setViewParams(_zoom, _panX, _panY);

        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 先画 GPU 图像
        _renderer.render(display_w, display_h);

        // 再画 ImGui
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(_window);
    }
}

void ImageApp::render_ui()
{
    ImGui::Begin("Controls");

    ImGui::InputText("FITS Path", &_currentPath);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
        open_file_dialog();

    if (ImGui::Button("Load FITS"))
    {
        if (!_currentPath.empty())
            load_fits_file(_currentPath);
    }

    // Bayer 选择
    const char* patterns[] = {"None", "RGGB", "BGGR", "GRBG", "GBRG"};
    int currentPattern = static_cast<int>(_bayerHint);
    bool bayerChanged = false;
    if (ImGui::Combo("Bayer", &currentPattern, patterns, IM_ARRAYSIZE(patterns)))
    {
        BayerPattern newPattern = static_cast<BayerPattern>(currentPattern);
        if (newPattern != _bayerHint)
        {
            _bayerHint = newPattern;
            bayerChanged = true;
        }
    }

    ImGui::Separator();

    // === 拉伸模式 ===
    const char* stretchModes[] = {
        "Linear",
        "Arcsinh",
        "Log",
        "Sqrt"
    };
    bool stretchModeChanged = ImGui::Combo("Stretch mode",
                                           &_stretchMode,
                                           stretchModes,
                                           IM_ARRAYSIZE(stretchModes));
    if (stretchModeChanged)
    {
        _renderer.setStretchMode(_stretchMode);
    }

    // === Auto stretch 参数 ===
    bool autoParamsChanged = false;

    if (ImGui::Checkbox("Auto Stretch", &_autoStretch))
        autoParamsChanged = true;

    if (ImGui::SliderFloat("Black clip %", &_blackClip, 0.0f, 5.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("White clip %", &_whiteClip, 0.0f, 5.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("Stretch strength", &_stretchStrength, 1.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    // 拉伸模式改变时，也认为 auto 参数变了（导出时会用）
    if (stretchModeChanged)
        autoParamsChanged = true;

    if (autoParamsChanged && _hasImage)
    {
        recompute_auto_params();
        _renderer.setAutoParams(_autoStretch, _autoLow, _autoHigh, _stretchStrength);
    }

    ImGui::Separator();

    // === 手动曲线 ===
    bool curveChanged = false;

    if (ImGui::Checkbox("Use manual curve", &_useManualCurve))
        curveChanged = true;

    if (ImGui::SliderFloat("Curve black", &_curveBlack, 0.0f, 0.5f))
        if (ImGui::IsItemEdited()) curveChanged = true;

    if (ImGui::SliderFloat("Curve white", &_curveWhite, 0.5f, 1.0f))
        if (ImGui::IsItemEdited()) curveChanged = true;

    if (ImGui::SliderFloat("Curve gamma", &_curveGamma, 0.1f, 5.0f))
        if (ImGui::IsItemEdited()) curveChanged = true;

    // 曲线预览
    {
        const int N = 256;
        static float curve[N];
        for (int i = 0; i < N; ++i)
        {
            float x = (float)i / (float)(N - 1);
            curve[i] = tone_curve(x, _curveBlack, _curveWhite, _curveGamma);
        }
        ImGui::PlotLines("Tone Curve", curve, N, 0, nullptr, 0.0f, 1.0f, ImVec2(0, 80));
    }

    if (curveChanged)
        _renderer.setCurveParams(_useManualCurve, _curveBlack, _curveWhite, _curveGamma);

    ImGui::Separator();

    // === 白平衡 ===
    bool wbChanged = false;

    if (ImGui::SliderFloat("R gain", &_wbR, 0.1f, 4.0f))
        wbChanged = true;
    if (ImGui::SliderFloat("G gain", &_wbG, 0.1f, 4.0f))
        wbChanged = true;
    if (ImGui::SliderFloat("B gain", &_wbB, 0.1f, 4.0f))
        wbChanged = true;

    if (wbChanged)
        _renderer.setWhiteBalance(_wbR, _wbG, _wbB);

    ImGui::Separator();
    if (ImGui::Button("Export PNG"))
    {
        if (_hasImage)
            export_png("output.png");
    }

    ImGui::End();

    if (_showFileDialog)
        render_file_dialog();

    // === 拜耳切换后，自动重新加载当前文件 ===
    if (bayerChanged && !_currentPath.empty())
    {
        _renderer.setBayerPattern(static_cast<int>(_bayerHint));
    }
}

// ---------- 文件对话 ----------
void ImageApp::open_file_dialog()
{
    try
    {
        if (!_currentPath.empty())
        {
            fs::path p(_currentPath);
            if (fs::is_directory(p))
                _fileDialogDir = p.string();
            else if (p.has_parent_path())
                _fileDialogDir = p.parent_path().string();
        }
        else
        {
            _fileDialogDir = fs::current_path().string();
        }
    }
    catch (...)
    {
        _fileDialogDir = ".";
    }

    _fileListDirty = true;
    _showFileDialog = true;
}

void ImageApp::refresh_file_list()
{
    _fileEntries.clear();
    try
    {
        for (auto& entry : fs::directory_iterator(_fileDialogDir))
            _fileEntries.push_back(entry.path().filename().string());

        std::sort(_fileEntries.begin(), _fileEntries.end());
    }
    catch (const std::exception& e)
    {
        std::cerr << "refresh_file_list error: " << e.what() << "\n";
    }
    _fileListDirty = false;
}

void ImageApp::render_file_dialog()
{
    if (!_showFileDialog) return;
    if (_fileListDirty)  refresh_file_list();

    ImGui::Begin("Open FITS", &_showFileDialog);

    ImGui::Text("Directory: %s", _fileDialogDir.c_str());

    if (ImGui::Button("Up"))
    {
        try
        {
            fs::path p(_fileDialogDir);
            if (p.has_parent_path())
            {
                _fileDialogDir = p.parent_path().string();
                _fileListDirty = true;
            }
        }
        catch (...) {}
    }

    ImGui::Separator();

    ImGui::BeginChild("file_list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);

    for (const auto& name : _fileEntries)
    {
        fs::path p = fs::path(_fileDialogDir) / name;
        bool isDir = false;
        try { isDir = fs::is_directory(p); } catch (...) {}

        if (isDir)
            ImGui::Text("[D] %s", name.c_str());
        else
            ImGui::Text("%s", name.c_str());

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        {
            if (isDir)
            {
                _fileDialogDir = p.string();
                _fileListDirty = true;
            }
            else
            {
                _currentPath = p.string();
                _showFileDialog = false;
            }
        }
    }

    ImGui::EndChild();

    if (ImGui::Button("Open"))
    {
        if (!_currentPath.empty())
        {
            load_fits_file(_currentPath);
            _showFileDialog = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        _showFileDialog = false;

    ImGui::End();
}

// ---------- 图像 & 拉伸 ----------

void ImageApp::load_fits_file(const std::string& path)
{
    FitsImage img;
    if (!load_fits(path, img, _bayerHint))
    {
        std::cerr << "Failed to load " << path << "\n";
        return;
    }

    _fits = img;

    // === 1. CPU 去拜耳：用于亮度统计 & 导出 PNG ===
    FitsImage debayered;
    if (!debayer_bilinear(_fits, debayered))
    {
        // 灰度兜底
        debayered = _fits;
        debayered.channels = 3;
        debayered.bayer = BayerPattern::NONE;
        debayered.rgb.resize(static_cast<size_t>(debayered.width) * debayered.height * 3);

        double mn = 0.0, mx = 1.0;
        if (!_fits.raw.empty())
        {
            auto [itMin, itMax] = std::minmax_element(_fits.raw.begin(), _fits.raw.end());
            mn = *itMin;
            mx = *itMax;
            if (mn == mx)
            {
                mn = 0.0;
                mx = 1.0;
            }
        }
        double range = mx - mn;

        for (int y = 0; y < debayered.height; ++y)
        {
            for (int x = 0; x < debayered.width; ++x)
            {
                size_t idx = static_cast<size_t>(y) * debayered.width + x;
                float v = static_cast<float>((_fits.raw[idx] - mn) / range);
                v = clamp01(v);
                debayered.rgb[idx * 3 + 0] = v;
                debayered.rgb[idx * 3 + 1] = v;
                debayered.rgb[idx * 3 + 2] = v;
            }
        }
    }

    _linearRgb = debayered.rgb;
    _imgWidth  = debayered.width;
    _imgHeight = debayered.height;
    _hasImage  = !_linearRgb.empty();

    // 重置视图状态
    _zoom = 1.0f;
    _panX = 0.0f;
    _panY = 0.0f;

    // === 2. 亮度统计 & auto stretch 参数 ===
    build_luminance_stats();
    recompute_auto_params();

    // === 3. 构造 Bayer 归一化数据，上传给 GPU 去拜耳 ===
    std::vector<float> bayerNorm;
    bayerNorm.resize(_fits.raw.size());

    if (!_fits.raw.empty())
    {
        auto [itMin, itMax] = std::minmax_element(_fits.raw.begin(), _fits.raw.end());
        double mn = *itMin;
        double mx = *itMax;
        if (mn == mx)
        {
            mn = 0.0;
            mx = 1.0;
        }
        double range = mx - mn;

        for (size_t i = 0; i < _fits.raw.size(); ++i)
        {
            float v = static_cast<float>((_fits.raw[i] - mn) / range);
            bayerNorm[i] = clamp01(v);
        }
    }

    _renderer.uploadBaseTexture(bayerNorm, _fits.width, _fits.height);
    _renderer.setBayerPattern(static_cast<int>(_bayerHint));
    _renderer.setAutoParams(_autoStretch, _autoLow, _autoHigh, _stretchStrength);
    _renderer.setCurveParams(_useManualCurve, _curveBlack, _curveWhite, _curveGamma);
    _renderer.setStretchMode(_stretchMode);
    _renderer.setWhiteBalance(_wbR, _wbG, _wbB);
}

void ImageApp::build_luminance_stats()
{
    _lum.clear();
    _lumSorted.clear();
    _medianLum = 0.0f;
    _madLum    = 0.0f;

    if (_linearRgb.empty())
        return;

    size_t pixels = _linearRgb.size() / 3;
    _lum.resize(pixels);

    for (size_t i = 0; i < pixels; ++i)
    {
        float r = _linearRgb[i * 3 + 0];
        float g = _linearRgb[i * 3 + 1];
        float b = _linearRgb[i * 3 + 2];
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        _lum[i] = clamp01(l);
    }

    _lumSorted = _lum;
    std::sort(_lumSorted.begin(), _lumSorted.end());

    size_t n = _lumSorted.size();
    if (n == 0)
        return;

    size_t mid = n / 2;
    if (n % 2 == 0)
        _medianLum = 0.5f * (_lumSorted[mid - 1] + _lumSorted[mid]);
    else
        _medianLum = _lumSorted[mid];

    std::vector<float> devs(n);
    for (size_t i = 0; i < n; ++i)
        devs[i] = std::fabs(_lum[i] - _medianLum);
    std::sort(devs.begin(), devs.end());
    if (n % 2 == 0)
        _madLum = 0.5f * (devs[mid - 1] + devs[mid]);
    else
        _madLum = devs[mid];

    if (_madLum < 1e-6f)
        _madLum = 1e-6f;
}

void ImageApp::recompute_auto_params()
{
    if (!_autoStretch || _lumSorted.empty())
    {
        _autoLow  = 0.0f;
        _autoHigh = 1.0f;
        return;
    }

    size_t n = _lumSorted.size();
    if (n == 0)
        return;

    auto clampPercent = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 100.0f) return 100.0f;
        return v;
    };

    float bc = clampPercent(_blackClip);
    float wc = clampPercent(_whiteClip);

    float pLow  = bc / 100.0f;
    float pHigh = (100.0f - wc) / 100.0f;

    size_t idxLow  = static_cast<size_t>(pLow  * (n - 1));
    size_t idxHigh = static_cast<size_t>(pHigh * (n - 1));
    if (idxLow >= n) idxLow = n - 1;
    if (idxHigh >= n) idxHigh = n - 1;
    if (idxLow > idxHigh) idxLow = 0;

    float lowP  = _lumSorted[idxLow];
    float highP = _lumSorted[idxHigh];

    const float kSigma = 1.5f;
    float candidateLow = clamp01(_medianLum - kSigma * _madLum);

    float low  = std::max(candidateLow, lowP);
    float high = std::max(highP, low + 1e-3f);

    if (high <= low + 1e-4f)
    {
        low  = lowP;
        high = std::max(highP, low + 1e-3f);
    }

    _autoLow  = low;
    _autoHigh = high;
}

// ---------- 导出 PNG：CPU 模拟 shader ----------

void ImageApp::compute_processed_cpu(std::vector<float>& outRgb)
{
    outRgb.clear();
    if (!_hasImage || _linearRgb.empty())
        return;

    outRgb.resize(_linearRgb.size());

    float low  = _autoLow;
    float high = _autoHigh;
    float range = std::max(high - low, 1e-3f);

    float s = std::max(_stretchStrength, 1.0f);
    float denom = std::asinh(s);
    if (denom < 1e-6f) denom = 1e-6f;

    for (size_t i = 0; i < _linearRgb.size(); i += 3)
    {
        float r = _linearRgb[i + 0];
        float g = _linearRgb[i + 1];
        float b = _linearRgb[i + 2];

        // 白平衡
        r *= _wbR;
        g *= _wbG;
        b *= _wbB;
        r = clamp01(r);
        g = clamp01(g);
        b = clamp01(b);

        if (_autoStretch)
        {
            r = (r - low) / range;
            g = (g - low) / range;
            b = (b - low) / range;

            r = clamp01(r);
            g = clamp01(g);
            b = clamp01(b);

            if (_stretchMode == 0)
            {
                // Linear
            }
            else if (_stretchMode == 1)
            {
                // asinh
                r = std::asinh(s * r) / denom;
                g = std::asinh(s * g) / denom;
                b = std::asinh(s * b) / denom;
            }
            else if (_stretchMode == 2)
            {
                // log
                float k = s;
                float dlog = std::log(1.0f + k);
                r = std::log(1.0f + k * r) / dlog;
                g = std::log(1.0f + k * g) / dlog;
                b = std::log(1.0f + k * b) / dlog;
            }
            else if (_stretchMode == 3)
            {
                // sqrt
                r = std::sqrt(r);
                g = std::sqrt(g);
                b = std::sqrt(b);
            }

            r = clamp01(r);
            g = clamp01(g);
            b = clamp01(b);
        }

        if (_useManualCurve)
        {
            r = tone_curve(r, _curveBlack, _curveWhite, _curveGamma);
            g = tone_curve(g, _curveBlack, _curveWhite, _curveGamma);
            b = tone_curve(b, _curveBlack, _curveWhite, _curveGamma);
        }

        outRgb[i + 0] = r;
        outRgb[i + 1] = g;
        outRgb[i + 2] = b;
    }
}

void ImageApp::export_png(const std::string& path)
{
    if (!_hasImage)
        return;

    std::vector<float> rgb;
    compute_processed_cpu(rgb);
    if (rgb.empty())
        return;

    std::vector<unsigned char> u8 = rgb_to_u8(rgb, _imgWidth, _imgHeight);

    int stride = _imgWidth * 3;
    if (!stbi_write_png(path.c_str(), _imgWidth, _imgHeight, 3,
                        u8.data(), stride))
        std::cerr << "Failed to write png: " << path << "\n";
    else
        std::cout << "PNG saved to " << path << "\n";
}
