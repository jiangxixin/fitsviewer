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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

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
        return false;
    }

    if (!_renderer.init())
    {
        std::cerr << "FitsRenderer init failed\n";
        return false;
    }

    // ImGui 初始化 + Docking + 默认字体
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(_window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // 初始参数
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
        _fileDialogDir = fs::current_path().string();
    }
    catch (...)
    {
        _fileDialogDir = ".";
    }
    _fileListDirty = true;

    return true;
}

void ImguiApp::shutdown()
{
    _renderer.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();

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

    // 一次性初始化 Dock 布局
    static bool firstTime = true;
    if (firstTime)
    {
        firstTime = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockFlags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

        ImGuiID dock_main_id = dockspace_id;

        // 左侧 25% → Controls + Histogram
        ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(
            dock_main_id,
            ImGuiDir_Left,
            0.25f,
            nullptr,
            &dock_main_id);

        ImGuiID dock_left_bottom_id = ImGui::DockBuilderSplitNode(
            dock_left_id,
            ImGuiDir_Down,
            0.40f,   // 下部 40% 给 Histogram
            nullptr,
            &dock_left_id);

        // 中间再切右 30% 给 Stack
        ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(
            dock_main_id,
            ImGuiDir_Right,
            0.30f,
            nullptr,
            &dock_main_id);

        // dock_left_id        -> Controls
        // dock_left_bottom_id -> Histogram
        // dock_main_id        -> Image
        // dock_right_id       -> Stack

        ImGui::DockBuilderDockWindow("Controls",  dock_left_id);
        ImGui::DockBuilderDockWindow("Histogram", dock_left_bottom_id);
        ImGui::DockBuilderDockWindow("Image",     dock_main_id);
        ImGui::DockBuilderDockWindow("Stack",     dock_right_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockFlags);

    ImGui::End();

    // ===== Controls 窗口 =====
    ImGui::Begin("Controls");

    // 路径输入 + Browse
    ImGui::InputText("FITS Path", &_currentPath);
    if (ImGui::Button("Browse..."))
        open_file_dialog();

    ImGui::Separator();

    // Bayer 选择
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

    ImGui::Separator();

    // Stretch mode
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

    // Auto Stretch 参数
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

    ImGui::Separator();

    // 视图 Scale
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

    ImGui::Separator();

    // 白平衡 + Auto WB
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

    ImGui::Separator();

    // 导出 PNG
    if (ImGui::Button("Export PNG"))
    {
        _exportJustSucceeded = false;
        if (_hasImage)
        {
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

            std::vector<unsigned char> rgb;
            int w = 0, h = 0;
            if (_renderer.renderToImage(rgb, w, h))
            {
                int stride = w * 3;
                if (stbi_write_png(outPath.string().c_str(), w, h, 3,
                                   rgb.data(), stride))
                {
                    _lastExportPath = outPath.string();
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
                           "导出成功: %s", _lastExportPath.c_str());
    }

    ImGui::End(); // Controls

    // ===== Histogram 窗口 =====
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

    // ===== Image 窗口：预览纹理 + 右键平移 =====
    ImGuiWindowFlags imageFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("Image", nullptr, imageFlags))
    {
        if (_hasImage)
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            int texW = (int)avail.x;
            int texH = (int)avail.y;

            if (texW > 0 && texH > 0)
            {
                ImGuiIO& io2 = ImGui::GetIO();

                // 在 Image 窗口内按住右键拖动，改变视图平移
                if (ImGui::IsWindowHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
                {
                    ImVec2 d = io2.MouseDelta;
                    float dx = -d.x / (float)texW;
                    float dy =  d.y / (float)texH;
                    _view.panX += dx * _view.scale;
                    _view.panY += dy * _view.scale;
                }

                _renderer.setViewParams(_view);

                if (_renderer.renderPreview(texW, texH))
                {
                    unsigned int texId = _renderer.previewTextureId();
                    if (texId != 0)
                    {
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

    // ===== Stack 独立窗口 =====
    render_stack_window();

    // ===== 文件对话框 =====
    if (_showFileDialog)
        render_file_dialog();
}

bool ImguiApp::loadCurrentFits()
{
    if (_currentPath.empty())
        return false;

    if (_renderer.loadFits(_currentPath, _bayer))
    {
        _hasImage = true;
        _renderer.setStretchParams(_stretch);
        _renderer.setWhiteBalance(_wb);

        // 这里可以选择是否自动 WB；现在只保留手动按钮
        // if (_renderer.computeAutoWhiteBalance())
        //     _wb = _renderer.whiteBalance();

        _renderer.recomputeAutoStretch();
        _histogram.clear();
        _renderer.getLumaHistogram(_histogram);

        return true;
    }

    return false;
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

    // 当前文件信息
    if (_currentPath.empty())
    {
        ImGui::TextUnformatted("请在 Controls 中选择 FITS 文件，或通过 Browse 打开。");
    }
    else
    {
        ImGui::Text("当前文件:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s", _currentPath.c_str());
    }

    ImGui::Separator();

    // 添加当前文件到 stack 列表
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
    else
    {
        ImGui::TextDisabled("（当前路径为空，无法添加到 Light/Dark/Flat/Bias 列表）");
    }

    // 文件列表
    ImGui::Separator();
    ImGui::Text("待叠加文件列表：");
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
    }

    ImGui::EndChild();

    // 删除 / 清空 按钮
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
        // 日志保留或清空看你需求，这里保留
    }

    ImGui::Separator();

    // 校准配置
    kty::CalibConfig calibCfg = _stack.calibConfig();
    ImGui::Checkbox("Use Bias", &calibCfg.useBias);
    ImGui::Checkbox("Use Dark", &calibCfg.useDark);
    ImGui::Checkbox("Use Flat", &calibCfg.useFlat);
    _stack.setCalibConfig(calibCfg);

    ImGui::Separator();

    // 运行叠加 / 重置工程
    if (ImGui::Button("Run Stack"))
    {
        _stackLog.clear();

        if (!_stack.buildMasters())
        {
            _stackLog += "buildMasters failed.\n";
        }
        else
        {
            kty::StackResult res;
            if (_stack.runStack(res))
            {
                _stackLog += res.log;

                // TODO: 将来可以把 res.finalImage 喂给 FitsRenderer 做预览
                // 比如新增 FitsRenderer::loadFromFitsImage(res.finalImage)
            }
            else
            {
                _stackLog += "runStack failed.\n";
            }
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

    // Log 区域：始终显示
    ImGui::Separator();
    ImGui::TextUnformatted("Stack log:");
    ImGui::BeginChild("stack_log_child", ImVec2(0, 180), true);

    if (_stackLog.empty())
        ImGui::TextDisabled("（尚无日志）");
    else
        ImGui::TextUnformatted(_stackLog.c_str());

    ImGui::EndChild();

    ImGui::End();
}

void ImguiApp::open_file_dialog()
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

    _fileListDirty  = true;
    _showFileDialog = true;
    _selectedFileIndex = -1;
}

void ImguiApp::refresh_file_list()
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

void ImguiApp::render_file_dialog()
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
        fs::path p = fs::path(_fileDialogDir) / name;

        bool isDir = false;
        try { isDir = fs::is_directory(p); } catch (...) {}

        bool selected = (i == _selectedFileIndex);
        std::string label = isDir ? "[D] " + name : name;

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            _selectedFileIndex = i;

            if (!isDir)
            {
                _currentPath = p.string();
            }

            if (ImGui::IsMouseDoubleClicked(0))
            {
                if (isDir)
                {
                    _fileDialogDir = p.string();
                    _fileListDirty = true;
                    _selectedFileIndex = -1;
                }
                else
                {
                    _currentPath = p.string();
                    if (loadCurrentFits())
                        _showFileDialog = false;
                }
            }
        }
    }

    ImGui::EndChild();

    if (ImGui::Button("Open"))
    {
        if (loadCurrentFits())
            _showFileDialog = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        _showFileDialog = false;

    ImGui::End();
}
