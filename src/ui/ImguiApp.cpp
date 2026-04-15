#include "ImguiApp.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

static std::string path_to_utf8(const fs::path& p)
{
    return p.u8string();
}

static fs::path utf8_to_path(const std::string& s)
{
    return fs::u8path(s);
}

static bool setup_imgui_fonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Prefer a CJK-capable font so UTF-8 Chinese filenames render correctly.
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesChineseFull();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;

    std::vector<fs::path> candidates;
#if defined(__APPLE__)
    candidates = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/Library/Fonts/Arial Unicode.ttf"
    };
#elif defined(_WIN32)
    candidates = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc"
    };
#else
    candidates = {
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"
    };
#endif

    for (const auto& p : candidates)
    {
        if (!fs::exists(p))
            continue;
        if (io.Fonts->AddFontFromFileTTF(path_to_utf8(p).c_str(), 13.0f, &cfg, glyphRanges))
            return true;
    }

    io.Fonts->AddFontDefault();
    return false;
}

ImguiApp::ImguiApp() {}
ImguiApp::~ImguiApp()
{
    shutdown();
}

bool ImguiApp::init()
{
    if (!glfwInit())
    {
        std::cerr << "Failed to init GLFW\n";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    _window = glfwCreateWindow(1280, 720, "FITS Viewer (ImGui Docking)", nullptr, nullptr);
    if (!_window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(_window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to init GLAD\n";
        shutdown();
        return false;
    }

    if (!_renderer.init())
    {
        std::cerr << "FitsRenderer init failed\n";
        shutdown();
        return false;
    }

    // ImGui init + docking
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (!setup_imgui_fonts())
        std::cerr << "Warning: no CJK font found, Chinese text may render as garbled boxes.\n";
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(_window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Initial parameters
    _stretch.autoStretch  = true;
    _stretch.blackClip    = 0.1f;
    _stretch.whiteClip    = 0.1f;
    _stretch.strength     = 5.0f;
    _stretch.mode         = kty::StretchMode::Asinh;

    _wb.r = _wb.g = _wb.b = 1.0f;

    _view.scale = 1.0f;
    _view.panX  = 0.0f;
    _view.panY  = 0.0f;

    try
    {
        _currentDir    = path_to_utf8(fs::current_path());
        _fileDialogDir = _currentDir;
    }
    catch (...)
    {
        _currentDir    = ".";
        _fileDialogDir = ".";
    }
    _fileListDirty = true;

    // First scan of current folder
    refreshDirFits();
    loadDirFitsCurrent();

    return true;
}

void ImguiApp::shutdown()
{
    _renderer.shutdown();

    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    if (_window)
    {
        glfwDestroyWindow(_window);
        _window = nullptr;
    }
    glfwTerminate();
}

void ImguiApp::run()
{
    if (!_window)
        return;

    while (!glfwWindowShouldClose(_window))
        frame();
}

void ImguiApp::frame()
{
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    render_ui();

    ImGui::Render();

    int fb_w, fb_h;
    glfwGetFramebufferSize(_window, &fb_w, &fb_h);

    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(_window);
}

bool ImguiApp::loadCurrentFits()
{
    if (_currentPath.empty())
    {
        _hasImage = false;
        _histogram.clear();
        return false;
    }

    if (_renderer.loadFits(_currentPath, _bayer))
    {
        _hasImage = true;
        _renderer.setStretchParams(_stretch);
        _renderer.setWhiteBalance(_wb);

        _renderer.recomputeAutoStretch();
        _histogram.clear();
        _renderer.getLumaHistogram(_histogram);

        return true;
    }

    _hasImage = false;
    _histogram.clear();
    return false;
}

// Scan current folder for FITS files
void ImguiApp::refreshDirFits()
{
    _dirFits.clear();
    _dirFitsIndex = -1;

    if (_currentDir.empty())
        return;

    try
    {
        for (auto& entry : fs::directory_iterator(utf8_to_path(_currentDir)))
        {
            if (!entry.is_regular_file())
                continue;

            const std::string name = path_to_utf8(entry.path().filename());
            if (!name.empty() && name[0] == '.')
                continue;

            auto ext = entry.path().extension().string();
            std::string extLower = ext;
            std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (extLower == ".fits" || extLower == ".fit" || extLower == ".fts")
                _dirFits.push_back(path_to_utf8(entry.path()));
        }

        std::sort(_dirFits.begin(), _dirFits.end());
        if (!_dirFits.empty())
            _dirFitsIndex = 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "refreshDirFits error: " << e.what() << "\n";
    }
}

bool ImguiApp::loadDirFitsCurrent()
{
    if (_dirFitsIndex < 0 || _dirFitsIndex >= (int)_dirFits.size())
        return false;

    _currentPath = _dirFits[_dirFitsIndex];
    return loadCurrentFits();
}

void ImguiApp::browseDirFits(int delta)
{
    if (_dirFits.empty())
        return;

    int n = (int)_dirFits.size();
    int idx = _dirFitsIndex;
    if (idx < 0) idx = 0;

    idx += delta;
    if (idx < 0)  idx = 0;
    if (idx >= n) idx = n - 1;

    if (idx != _dirFitsIndex)
    {
        _dirFitsIndex = idx;
        loadDirFitsCurrent();
    }
}

void ImguiApp::render_ui()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostWindowFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##DockSpaceHost", nullptr, hostWindowFlags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;

    // Build docking layout once
    static bool firstTime = true;
    if (firstTime)
    {
        firstTime = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockFlags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

        ImGuiID dock_main_id = dockspace_id;

        // Left 25% → File Browse + Histogram
        ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(
            dock_main_id,
            ImGuiDir_Left,
            0.25f,
            nullptr,
            &dock_main_id);

        ImGuiID dock_left_bottom_id = ImGui::DockBuilderSplitNode(
            dock_left_id,
            ImGuiDir_Down,
            0.40f,
            nullptr,
            &dock_left_id);

        // Middle split right 30% for Controls + Stack
        ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(
            dock_main_id,
            ImGuiDir_Right,
            0.30f,
            nullptr,
            &dock_main_id);

        // dock_left_id        -> File Browse
        // dock_left_bottom_id -> Histogram
        // dock_main_id        -> Image
        // dock_right_id       -> Controls & Stack (tab)

        ImGui::DockBuilderDockWindow("File Browse", dock_left_id);
        ImGui::DockBuilderDockWindow("Histogram",  dock_left_bottom_id);
        ImGui::DockBuilderDockWindow("Image",      dock_main_id);
        ImGui::DockBuilderDockWindow("Controls",   dock_right_id);
        ImGui::DockBuilderDockWindow("Stack",      dock_right_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockFlags);

    ImGui::End();

    // ===== Windows =====

    // File browse (left, with file list + keyboard focus)
    render_file_browse_window();

    // Controls (right tab)
    ImGui::Begin("Controls");

    ImGui::TextUnformatted("Image Controls");

    // Bayer selection
    ImGui::Separator();
    const char* patterns[] = {"None", "RGGB", "BGGR", "GRBG", "GBRG"};
    int bayerIndex = static_cast<int>(_bayer);
    bool bayerChanged = false;
    if (ImGui::Combo("Bayer", &bayerIndex, patterns, IM_ARRAYSIZE(patterns)))
    {
        kty::BayerPattern newB = static_cast<kty::BayerPattern>(bayerIndex);
        if (newB != _bayer)
        {
            _bayer = newB;
            bayerChanged = true;
            if (_hasImage)
                _renderer.setBayerPattern(_bayer);
        }
    }

    // Stretch
    ImGui::Separator();
    const char* stretchModes[] = {"Linear", "Arcsinh", "Log", "Sqrt"};
    int stretchIndex = static_cast<int>(_stretch.mode);
    bool stretchModeChanged = ImGui::Combo("Stretch mode", &stretchIndex,
                                           stretchModes, IM_ARRAYSIZE(stretchModes));
    if (stretchModeChanged)
    {
        _stretch.mode = static_cast<kty::StretchMode>(stretchIndex);
        if (_hasImage)
            _renderer.setStretchParams(_stretch);
    }

    bool autoParamsChanged = false;

    if (ImGui::Checkbox("Auto Stretch", &_stretch.autoStretch))
        autoParamsChanged = true;

    if (ImGui::SliderFloat("Black clip %", &_stretch.blackClip, 0.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("White clip %", &_stretch.whiteClip, 0.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (ImGui::SliderFloat("Stretch strength", &_stretch.strength, 1.0f, 20.0f))
        if (ImGui::IsItemEdited()) autoParamsChanged = true;

    if (stretchModeChanged) autoParamsChanged = true;
    if (bayerChanged)       autoParamsChanged = true;

    if (autoParamsChanged && _hasImage)
    {
        _renderer.setStretchParams(_stretch);
        if (_renderer.recomputeAutoStretch())
        {
            _histogram.clear();
            _renderer.getLumaHistogram(_histogram);
        }
    }

    // View scale
    ImGui::Separator();
    {
        float zoomMin = 0.1f, zoomMax = 20.0f;
        if (ImGui::SliderFloat("Scale", &_view.scale, zoomMin, zoomMax, "%.2f",
                               ImGuiSliderFlags_Logarithmic))
        {
            if (_view.scale < zoomMin) _view.scale = zoomMin;
            if (_view.scale > zoomMax) _view.scale = zoomMax;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset View"))
        {
            _view.scale = 1.0f;
            _view.panX  = 0.0f;
            _view.panY  = 0.0f;
        }
    }

    // White balance
    ImGui::Separator();
    bool wbChanged = false;
    if (ImGui::SliderFloat("R gain", &_wb.r, 0.1f, 4.0f)) wbChanged = true;
    if (ImGui::SliderFloat("G gain", &_wb.g, 0.1f, 4.0f)) wbChanged = true;
    if (ImGui::SliderFloat("B gain", &_wb.b, 0.1f, 4.0f)) wbChanged = true;

    if (ImGui::Button("Auto White Balance"))
    {
        if (_hasImage && _renderer.computeAutoWhiteBalance())
        {
            _wb = _renderer.whiteBalance();

            if (_renderer.recomputeAutoStretch())
            {
                _histogram.clear();
                _renderer.getLumaHistogram(_histogram);
            }
        }
    }

    if (wbChanged && _hasImage)
    {
        _renderer.setWhiteBalance(_wb);
        if (_renderer.recomputeAutoStretch())
        {
            _histogram.clear();
            _renderer.getLumaHistogram(_histogram);
        }
    }

    // Export PNG
    ImGui::Separator();
    if (ImGui::Button("Export PNG"))
    {
        _exportJustSucceeded = false;
        if (_hasImage)
        {
            fs::path inPath = utf8_to_path(_currentPath);
            fs::path outPath;
            if (!inPath.empty())
            {
                outPath = inPath;
                outPath.replace_extension(".png");
            }
            else
            {
                outPath = fs::current_path() / fs::u8path("output.png");
            }

            std::vector<unsigned char> rgb;
            int w = 0, h = 0;
            if (_renderer.renderToImage(rgb, w, h))
            {
                int stride = w * 3;
                const std::string outPathUtf8 = path_to_utf8(outPath);
                if (stbi_write_png(outPathUtf8.c_str(), w, h, 3,
                                   rgb.data(), stride))
                {
                    _lastExportPath = outPathUtf8;
                    _exportJustSucceeded = true;
                    std::cout << "PNG saved to " << _lastExportPath << "\n";
                }
                else
                {
                    std::cerr << "Failed to write png: " << outPath << "\n";
                }
            }
        }
    }

    if (_exportJustSucceeded && !_lastExportPath.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                           "Exported: %s", _lastExportPath.c_str());
    }

    ImGui::End(); // Controls

    // Histogram window
    if (ImGui::Begin("Histogram"))
    {
        if (!_histogram.empty())
        {
            ImGui::PlotHistogram("Luma",
                                 _histogram.data(),
                                 (int)_histogram.size(),
                                 0,
                                 nullptr,
                                 0.0f,
                                 1.0f,
                                 ImVec2(0, 120));
        }
        else
        {
            ImGui::TextUnformatted("No histogram yet.");
        }
    }
    ImGui::End();

    // Image window: preview texture + right-button pan
    ImGuiWindowFlags imageFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("Image", nullptr, imageFlags))
    {
        if (_hasImage)
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            int texW = 0;
            int texH = 0;

            const float imgW = static_cast<float>(_renderer.width());
            const float imgH = static_cast<float>(_renderer.height());
            if (avail.x > 1.0f && avail.y > 1.0f && imgW > 1.0f && imgH > 1.0f)
            {
                float scale = std::min(avail.x / imgW, avail.y / imgH);
                float drawW = imgW * scale;
                float drawH = imgH * scale;
                texW = std::max(1, (int)drawW);
                texH = std::max(1, (int)drawH);

                ImGuiIO& io2 = ImGui::GetIO();
                const bool hovered = ImGui::IsWindowHovered();

                if (hovered && io2.MouseWheel != 0.0f)
                {
                    const float factor = (io2.MouseWheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
                    _view.scale *= factor;
                    if (_view.scale < 0.1f) _view.scale = 0.1f;
                    if (_view.scale > 20.0f) _view.scale = 20.0f;
                }

                if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right))
                {
                    ImVec2 d = io2.MouseDelta;
                    float dx = -d.x / (float)texW;
                    float dy =  d.y / (float)texH;
                    float zoom = (_view.scale > 0.1f) ? _view.scale : 0.1f;
                    _view.panX += dx / zoom;
                    _view.panY += dy / zoom;
                }

                _renderer.setViewParams(_view);

                if (_renderer.renderPreview(texW, texH))
                {
                    unsigned int texId = _renderer.previewTextureId();
                    if (texId != 0)
                    {
                        ImVec2 pos = ImGui::GetCursorPos();
                        pos.x += (avail.x - (float)texW) * 0.5f;
                        pos.y += (avail.y - (float)texH) * 0.5f;
                        ImGui::SetCursorPos(pos);
                        ImGui::Image(
                            (ImTextureID)(intptr_t)texId,
                            ImVec2((float)texW, (float)texH),
                            ImVec2(0.0f, 1.0f),
                            ImVec2(1.0f, 0.0f)
                        );
                    }
                }
            }
        }
        else
        {
            ImGui::TextUnformatted("No image loaded.");
        }
    }
    ImGui::End();

    // Stack window (right tab)
    render_stack_window();

    // Folder selection dialog
    if (_showFileDialog)
        render_file_dialog();
}

void ImguiApp::render_file_browse_window()
{
    ImGui::Begin("File Browse");

    ImGui::TextUnformatted("Folder & FITS file list");

    ImGui::Separator();
    ImGui::InputText("Folder", &_currentDir, ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button("Browse Folder..."))
        open_file_dialog();
    ImGui::SameLine();
    if (ImGui::Button("Rescan") && !_currentDir.empty())
    {
        refreshDirFits();
        loadDirFitsCurrent();
    }

    ImGui::Separator();
    ImGui::Text("FITS files: %d", (int)_dirFits.size());

    // File list with keyboard focus
    ImGui::BeginChild("dir_fits_list", ImVec2(0, 0), true);

    bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (windowFocused)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            browseDirFits(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            browseDirFits(+1);
    }

    for (int i = 0; i < (int)_dirFits.size(); ++i)
    {
        fs::path p = utf8_to_path(_dirFits[i]);
        std::string label = path_to_utf8(p.filename());
        bool selected = (i == _dirFitsIndex);

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            _dirFitsIndex = i;
            loadDirFitsCurrent();
        }

        if (selected)
            ImGui::SetItemDefaultFocus();
    }

    if (_dirFits.empty())
    {
        ImGui::TextDisabled("No FITS file in this folder.");
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Use Up/Down arrow keys to preview.");
    }

    ImGui::EndChild();

    ImGui::End();
}

void ImguiApp::rebuildStackCoreFromUi()
{
    _stack.reset();

    _stackLightCount = _stackDarkCount = _stackFlatCount = _stackBiasCount = 0;

    for (const auto& item : _stackFiles)
    {
        _stack.addFrame(item.path, item.type);

        switch (item.type)
        {
        case kty::FrameType::Light: _stackLightCount++; break;
        case kty::FrameType::Dark:  _stackDarkCount++;  break;
        case kty::FrameType::Flat:  _stackFlatCount++;  break;
        case kty::FrameType::Bias:  _stackBiasCount++;  break;
        }
    }

    if (_stackSelectedIndex >= (int)_stackFiles.size())
        _stackSelectedIndex = (int)_stackFiles.size() - 1;
}

void ImguiApp::render_stack_window()
{
    ImGui::Begin("Stack");

    // Current file info
    if (_currentPath.empty())
    {
        ImGui::TextUnformatted("No FITS loaded. Select a folder in 'File Browse'.");
    }
    else
    {
        ImGui::Text("Current file:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", _currentPath.c_str());
    }

    ImGui::Separator();

    // Add current file to stack lists
    if (!_currentPath.empty())
    {
        if (ImGui::Button("Add as Light"))
        {
            _stackFiles.push_back({ _currentPath, kty::FrameType::Light });
            rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Dark"))
        {
            _stackFiles.push_back({ _currentPath, kty::FrameType::Dark });
            rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Flat"))
        {
            _stackFiles.push_back({ _currentPath, kty::FrameType::Flat });
            rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Bias"))
        {
            _stackFiles.push_back({ _currentPath, kty::FrameType::Bias });
            rebuildStackCoreFromUi();
        }
    }

    // File list for stacking
    ImGui::Separator();
    ImGui::Text("Frames to stack:");
    ImGui::Text("Lights: %d  Darks: %d  Flats: %d  Bias: %d",
                _stackLightCount, _stackDarkCount, _stackFlatCount, _stackBiasCount);

    ImGui::BeginChild("stack_file_list", ImVec2(0, 180), true);

    for (int i = 0; i < (int)_stackFiles.size(); ++i)
    {
        const auto& item = _stackFiles[i];

        const char* typeTag = "";
        switch (item.type)
        {
        case kty::FrameType::Light: typeTag = "[L]"; break;
        case kty::FrameType::Dark:  typeTag = "[D]"; break;
        case kty::FrameType::Flat:  typeTag = "[F]"; break;
        case kty::FrameType::Bias:  typeTag = "[B]"; break;
        }

        bool selected = (i == _stackSelectedIndex);
        std::string label = std::string(typeTag) + " " + item.path;

        if (ImGui::Selectable(label.c_str(), selected))
        {
            _stackSelectedIndex = i;
        }

        if (selected)
            ImGui::SetItemDefaultFocus();
    }

    if (_stackFiles.empty())
    {
        ImGui::TextDisabled("No frames added yet.");
    }

    ImGui::EndChild();

    // Remove / clear
    if (ImGui::Button("Remove Selected"))
    {
        if (_stackSelectedIndex >= 0 && _stackSelectedIndex < (int)_stackFiles.size())
        {
            _stackFiles.erase(_stackFiles.begin() + _stackSelectedIndex);
            _stackSelectedIndex = -1;
            rebuildStackCoreFromUi();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        _stackFiles.clear();
        _stackSelectedIndex = -1;
        _stack.reset();
        _stackLightCount = _stackDarkCount = _stackFlatCount = _stackBiasCount = 0;
        // keep log
    }

    ImGui::Separator();

    // Calibration config
    kty::CalibConfig calibCfg = _stack.calibConfig();
    ImGui::Checkbox("Use Bias", &calibCfg.useBias);
    ImGui::Checkbox("Use Dark", &calibCfg.useDark);
    ImGui::Checkbox("Use Flat", &calibCfg.useFlat);
    _stack.setCalibConfig(calibCfg);

    ImGui::Separator();

    // Run / reset stack
    if (ImGui::Button("Run Stack"))
    {
        _stackLog.clear();

        kty::StackResult res;
        if (_stack.runStack(res))
        {
            _stackLog += res.log;
            // TODO: later feed res.finalImage to FitsRenderer for preview
        }
        else
        {
            if (!res.log.empty())
                _stackLog += res.log;
            else
                _stackLog += "runStack failed.\n";
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset Stack Project"))
    {
        _stackFiles.clear();
        _stack.reset();
        _stackLightCount = _stackDarkCount = _stackFlatCount = _stackBiasCount = 0;
        _stackSelectedIndex = -1;
        _stackLog.clear();
    }

    // Log: always visible
    ImGui::Separator();
    ImGui::TextUnformatted("Stack log:");
    ImGui::BeginChild("stack_log_child", ImVec2(0, 180), true);

    if (_stackLog.empty())
        ImGui::TextDisabled("No log yet.");
    else
        ImGui::TextUnformatted(_stackLog.c_str());

    ImGui::EndChild();

    ImGui::End();
}

void ImguiApp::open_file_dialog()
{
    // Start from current folder
    try
    {
        if (!_currentDir.empty())
            _fileDialogDir = _currentDir;
        else
            _fileDialogDir = path_to_utf8(fs::current_path());
    }
    catch (...)
    {
        _fileDialogDir = ".";
    }

    _fileListDirty  = true;
    _showFileDialog = true;
    _selectedFileIndex = -1;
}

void ImguiApp::refresh_file_list()
{
    _fileEntries.clear();
    try
    {
        for (auto& entry : fs::directory_iterator(utf8_to_path(_fileDialogDir)))
        {
            const std::string name = path_to_utf8(entry.path().filename());
            if (!name.empty() && name[0] == '.')
                continue;
            _fileEntries.push_back(name);
        }

        std::sort(_fileEntries.begin(), _fileEntries.end());
    }
    catch (const std::exception& e)
    {
        std::cerr << "refresh_file_list error: " << e.what() << "\n";
    }
    _fileListDirty = false;
}

void ImguiApp::render_file_dialog()
{
    if (!_showFileDialog) return;
    if (_fileListDirty)  refresh_file_list();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.7f, io.DisplaySize.y * 0.7f),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(600, 400), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::Begin("Select Folder", &_showFileDialog);

    ImGui::Text("Directory: %s", _fileDialogDir.c_str());

    if (ImGui::Button("Up"))
    {
        try
        {
            fs::path p = utf8_to_path(_fileDialogDir);
            if (p.has_parent_path())
            {
                _fileDialogDir = path_to_utf8(p.parent_path());
                _fileListDirty = true;
                _selectedFileIndex = -1;
            }
        }
        catch (...) {}
    }

    ImGui::Separator();

    ImGui::BeginChild("file_list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);

    for (int i = 0; i < (int)_fileEntries.size(); ++i)
    {
        const std::string& name = _fileEntries[i];
        fs::path p = utf8_to_path(_fileDialogDir) / utf8_to_path(name);

        bool isDir = false;
        try { isDir = fs::is_directory(p); } catch (...) {}

        bool selected = (i == _selectedFileIndex);
        std::string label = isDir ? "[D] " + name : name;

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            _selectedFileIndex = i;

            if (ImGui::IsMouseDoubleClicked(0))
            {
                if (isDir)
                {
                    _fileDialogDir = path_to_utf8(p);
                    _fileListDirty = true;
                    _selectedFileIndex = -1;
                }
                // double-click file: we ignore, only folder matters
            }
        }
    }

    ImGui::EndChild();

    // Use this folder
    if (ImGui::Button("Use this folder"))
    {
        _currentDir = _fileDialogDir;
        refreshDirFits();
        loadDirFitsCurrent();
        _showFileDialog = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        _showFileDialog = false;

    ImGui::End();
}
