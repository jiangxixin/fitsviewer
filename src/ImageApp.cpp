#include "ImageApp.h"
#include "Debayer.h"
#include "Stretch.h"
#include "FitsImage.h"
#include "EmbeddedFont.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_internal.h>
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

// ===== App 自定义配置，写入 imgui.ini =====
struct AppSettings
{
    std::string lastDir;
    int  bayerPattern   = 1;   // 默认 RGGB
    int  stretchMode    = 1;   // 默认 Arcsinh
    float wbR           = 1.0f;
    float wbG           = 1.0f;
    float wbB           = 1.0f;
};

static AppSettings g_AppSettings;

static void AppSettings_ClearAll(ImGuiContext*, ImGuiSettingsHandler*)
{
    g_AppSettings = AppSettings{};
}

static void* AppSettings_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
{
    if (strcmp(name, "Main") == 0)
        return &g_AppSettings;
    return nullptr;
}

static void AppSettings_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
    if (strncmp(line, "LastDir=", 8) == 0)
    {
        g_AppSettings.lastDir = line + 8;
    }
    else if (sscanf(line, "Bayer=%d", &g_AppSettings.bayerPattern) == 1)
    {
    }
    else if (sscanf(line, "StretchMode=%d", &g_AppSettings.stretchMode) == 1)
    {
    }
    else if (sscanf(line, "WBR=%f", &g_AppSettings.wbR) == 1)
    {
    }
    else if (sscanf(line, "WBG=%f", &g_AppSettings.wbG) == 1)
    {
    }
    else if (sscanf(line, "WBB=%f", &g_AppSettings.wbB) == 1)
    {
    }
}

static void AppSettings_WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf)
{
    (void)ctx;
    out_buf->appendf("[%s][Main]\n", handler->TypeName);
    if (!g_AppSettings.lastDir.empty())
        out_buf->appendf("LastDir=%s\n", g_AppSettings.lastDir.c_str());

    out_buf->appendf("Bayer=%d\n", g_AppSettings.bayerPattern);
    out_buf->appendf("StretchMode=%d\n", g_AppSettings.stretchMode);
    out_buf->appendf("WBR=%f\n", g_AppSettings.wbR);
    out_buf->appendf("WBG=%f\n", g_AppSettings.wbG);
    out_buf->appendf("WBB=%f\n", g_AppSettings.wbB);
    out_buf->append("\n");
}
// ===== App 自定义配置结束 =====

ImageApp::ImageApp() {}
ImageApp::~ImageApp()
{
    //shutdown();
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

    // 注册 SettingsHandler
    {
        ImGuiSettingsHandler handler;
        handler.TypeName = "App";
        handler.TypeHash = ImHashStr("App");
        handler.ClearAllFn = AppSettings_ClearAll;
        handler.ReadInitFn = nullptr;
        handler.ReadOpenFn = AppSettings_ReadOpen;
        handler.ReadLineFn = AppSettings_ReadLine;
        handler.ApplyAllFn = nullptr;
        handler.WriteAllFn = AppSettings_WriteAll;
        ImGui::GetCurrentContext()->SettingsHandlers.push_back(handler);
    }

    // 加载内嵌中文字体
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        ImFont* mainFont = nullptr;

        if (g_NotoSansSC_compressed_size > 0)
        {
            ImFontConfig cfg;
            cfg.SizePixels = 13.0f;

            const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();

            mainFont = io.Fonts->AddFontFromMemoryCompressedTTF(
                (void*)g_NotoSansSC_compressed_data,
                g_NotoSansSC_compressed_size,
                13.0f,
                &cfg,
                ranges
            );
        }

        if (!mainFont)
        {
            std::cerr << "WARNING: embedded font not loaded, using default.\n";
            mainFont = io.Fonts->AddFontDefault();
        }

        io.FontDefault = mainFont;
    }

    ImGui_ImplGlfw_InitForOpenGL(_window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // 初始化 lastDir / Bayer / Stretch / WB
    try
    {
        if (!g_AppSettings.lastDir.empty() && fs::exists(g_AppSettings.lastDir))
            _fileDialogDir = g_AppSettings.lastDir;
        else
            _fileDialogDir = fs::current_path().string();
    }
    catch (...)
    {
        _fileDialogDir = ".";
    }
    _fileListDirty = true;

    _bayerHint   = static_cast<BayerPattern>(g_AppSettings.bayerPattern);
    _stretchMode = g_AppSettings.stretchMode;
    _wbR         = g_AppSettings.wbR;
    _wbG         = g_AppSettings.wbG;
    _wbB         = g_AppSettings.wbB;

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

        // 用 UI 滑块控制缩放，右键平移
        ImGuiIO& io = ImGui::GetIO();
        if (_hasImage)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                float dx = -io.MouseDelta.x / (float)display_w;
                float dy =  io.MouseDelta.y / (float)display_h;
                _panX += dx * _zoom;
                _panY += dy * _zoom;
            }
        }

        _renderer.setViewParams(_zoom, _panX, _panY);

        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        _renderer.render(display_w, display_h);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(_window);
    }
}

void ImageApp::render_ui()
{
    ImGui::Begin("Controls");

    // ===== 文件路径 / 打开 =====
    ImGui::InputText("FITS Path", &_currentPath);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
        open_file_dialog();

    if (ImGui::Button("Load FITS"))
    {
        if (!_currentPath.empty())
            load_fits_file(_currentPath);
    }

    // ===== Bayer 模式 =====
    const char* patterns[] = {"None", "RGGB", "BGGR", "GRBG", "GBRG"};
    int currentPattern = static_cast<int>(_bayerHint);
    bool bayerChanged = false;
    if (ImGui::Combo("Bayer", &currentPattern, patterns, IM_ARRAYSIZE(patterns)))
    {
        BayerPattern newPattern = static_cast<BayerPattern>(currentPattern);
        if (newPattern != _bayerHint)
        {
            _bayerHint = newPattern;
            g_AppSettings.bayerPattern = currentPattern;
            bayerChanged = true;
        }
    }

    ImGui::Separator();

    // ===== 拉伸模式 =====
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
        g_AppSettings.stretchMode = _stretchMode;
    }

    // ===== Auto Stretch 参数 =====
    bool autoParamsChanged = false;

    if (ImGui::Checkbox("Auto Stretch", &_autoStretch))
        autoParamsChanged = true;

    if (ImGui::SliderFloat("Black clip %", &_blackClip, 0.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("White clip %", &_whiteClip, 0.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("Stretch strength", &_stretchStrength, 1.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (stretchModeChanged)
        autoParamsChanged = true;

    // Bayer 改变后，也需要重新统计自动拉伸 & 直方图
    if (bayerChanged)
        autoParamsChanged = true;

    // ===== 调用 GPU 统计 auto 参数 + 更新直方图 =====
    if (autoParamsChanged && _hasImage)
    {
        float low = 0.0f, high = 1.0f;
        if (_renderer.computeAutoParamsGpu(_autoStretch, _blackClip, _whiteClip, low, high))
        {
            _autoLow  = low;
            _autoHigh = high;

            _histogram.clear();
            _renderer.getLuminanceHistogram(_histogram);
        }
        else
        {
            _autoLow  = 0.0f;
            _autoHigh = 1.0f;
            _histogram.clear();
        }

        _renderer.setAutoParams(_autoStretch, _autoLow, _autoHigh, _stretchStrength);
    }

    // ===== 实时亮度直方图（基于当前拉伸后的亮度） =====
    if (!_histogram.empty())
    {
        ImGui::Text("Luma Histogram");
        ImGui::PlotHistogram("##LumaHistogram",
                             _histogram.data(),
                             (int)_histogram.size(),
                             0,
                             nullptr,
                             0.0f,
                             1.0f,
                             ImVec2(0, 80));
    }

    ImGui::Separator();

    // ===== 手动 Tone Curve =====
    bool curveChanged = false;

    if (ImGui::Checkbox("Use manual curve", &_useManualCurve))
        curveChanged = true;

    if (ImGui::SliderFloat("Curve black", &_curveBlack, 0.0f, 0.5f))
        if (ImGui::IsItemEdited()) curveChanged = true;

    if (ImGui::SliderFloat("Curve white", &_curveWhite, 0.5f, 1.0f))
        if (ImGui::IsItemEdited()) curveChanged = true;

    if (ImGui::SliderFloat("Curve gamma", &_curveGamma, 0.1f, 5.0f))
        if (ImGui::IsItemEdited()) curveChanged = true;

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

    // ===== 视图 Scale 缩放 + Reset =====
    {
        float zoomMin = 0.1f, zoomMax = 20.0f;
        if (ImGui::SliderFloat("Scale", &_zoom, zoomMin, zoomMax, "%.2f", ImGuiSliderFlags_Logarithmic))
        {
            if (_zoom < zoomMin) _zoom = zoomMin;
            if (_zoom > zoomMax) _zoom = zoomMax;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset View"))
        {
            _zoom = 1.0f;
            _panX = 0.0f;
            _panY = 0.0f;
        }
    }

    ImGui::Separator();

    // ===== 白平衡 =====
    bool wbChanged = false;

    if (ImGui::SliderFloat("R gain", &_wbR, 0.1f, 4.0f))
        wbChanged = true;
    if (ImGui::SliderFloat("G gain", &_wbG, 0.1f, 4.0f))
        wbChanged = true;
    if (ImGui::SliderFloat("B gain", &_wbB, 0.1f, 4.0f))
        wbChanged = true;

    if (wbChanged)
    {
        _renderer.setWhiteBalance(_wbR, _wbG, _wbB);
        g_AppSettings.wbR = _wbR;
        g_AppSettings.wbG = _wbG;
        g_AppSettings.wbB = _wbB;

        // 白平衡改变后也重新跑 auto stretch + 直方图
        if (_hasImage)
        {
            float low = 0.0f, high = 1.0f;
            if (_renderer.computeAutoParamsGpu(_autoStretch, _blackClip, _whiteClip, low, high))
            {
                _autoLow  = low;
                _autoHigh = high;
                _histogram.clear();
                _renderer.getLuminanceHistogram(_histogram);
            }
            else
            {
                _autoLow  = 0.0f;
                _autoHigh = 1.0f;
                _histogram.clear();
            }
            _renderer.setAutoParams(_autoStretch, _autoLow, _autoHigh, _stretchStrength);
        }
    }

    ImGui::Separator();

    // ===== 导出 PNG（使用原文件名） =====
    if (ImGui::Button("Export PNG"))
    {
        _exportJustSucceeded = false;
        if (_hasImage)
            export_png("ignored");   // 实现里已经用 _currentPath 生成真正路径
    }

    if (_exportJustSucceeded && !_lastExportPath.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                           "导出成功: %s", _lastExportPath.c_str());
    }

    ImGui::End();

    // ===== 文件对话框 =====
    if (_showFileDialog)
        render_file_dialog();

    // Bayer 模式改变：即时更新 GPU Bayer pattern
    if (bayerChanged)
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
            if (!g_AppSettings.lastDir.empty() && fs::exists(g_AppSettings.lastDir))
                _fileDialogDir = g_AppSettings.lastDir;
            else
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

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.7f, io.DisplaySize.y * 0.7f),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(600, 400), ImVec2(FLT_MAX, FLT_MAX));

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

// ---------- 图像加载 ----------

void ImageApp::load_fits_file(const std::string& path)
{
    // 更新 lastDir
    try
    {
        fs::path p(path);
        if (fs::exists(p))
        {
            fs::path dir = p.has_parent_path() ? p.parent_path() : fs::current_path();
            _fileDialogDir = dir.string();
            g_AppSettings.lastDir = _fileDialogDir;
        }
    }
    catch (...) {}

    FitsImage img;
    if (!load_fits(path, img, _bayerHint))
    {
        std::cerr << "Failed to load " << path << "\n";
        return;
    }

    _fits = img;
    _imgWidth  = _fits.width;
    _imgHeight = _fits.height;
    _hasImage  = !_fits.raw.empty();

    _zoom = 1.0f;
    _panX = 0.0f;
    _panY = 0.0f;

    // 归一化 RAW 到 [0,1]，上传给 GPU
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
    _renderer.setWhiteBalance(_wbR, _wbG, _wbB);
    _renderer.setStretchMode(_stretchMode);

    // GPU 统计 auto stretch 参数 + 直方图
    float low = 0.0f, high = 1.0f;
    if (_renderer.computeAutoParamsGpu(_autoStretch, _blackClip, _whiteClip, low, high))
    {
        _autoLow  = low;
        _autoHigh = high;

        _histogram.clear();
        _renderer.getLuminanceHistogram(_histogram);
    }
    else
    {
        _autoLow  = 0.0f;
        _autoHigh = 1.0f;
        _histogram.clear();
    }

    _renderer.setAutoParams(_autoStretch, _autoLow, _autoHigh, _stretchStrength);
}

// ---------- 导出 PNG：完全用 GPU 渲染 ----------

void ImageApp::export_png(const std::string& /*path_unused*/)
{
    if (!_hasImage || _imgWidth <= 0 || _imgHeight <= 0)
        return;

    // 使用当前视图参数
    _renderer.setViewParams(_zoom, _panX, _panY);

    std::vector<unsigned char> rgb;
    if (!_renderer.renderToImage(_imgWidth, _imgHeight, rgb))
    {
        std::cerr << "Failed to render image for export\n";
        _exportJustSucceeded = false;
        return;
    }

    fs::path inPath(_currentPath);
    fs::path outPath;

    if (!inPath.empty())
    {
        outPath = inPath;
        outPath.replace_extension(".png");
    }
    else
    {
        outPath = fs::current_path() / "output.png";
    }

    std::string outStr = outPath.string();
    int stride = _imgWidth * 3;

    if (!stbi_write_png(outStr.c_str(), _imgWidth, _imgHeight, 3,
                        rgb.data(), stride))
    {
        std::cerr << "Failed to write png: " << outStr << "\n";
        _exportJustSucceeded = false;
    }
    else
    {
        _lastExportPath = outStr;
        _exportJustSucceeded = true;
        std::cout << "PNG saved to " << _lastExportPath << "\n";
    }
}
