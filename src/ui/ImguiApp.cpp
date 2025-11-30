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

    // === 一次性初始化 Dock 布局 ===
    static bool firstTime = true;
    if (firstTime)
    {
        firstTime = false;

        // 清空旧节点
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockFlags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

        // 从左侧切一块 25% 宽度给 Controls + Histogram
        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(
            dock_main_id,       // 要切割的节点
            ImGuiDir_Left,      // 从左侧切
            0.25f,              // 左侧占 25%
            nullptr,            // 返回左节点
            &dock_main_id       // 剩余的作为主区域
        );
        // 再把左侧上下分成 Controls（上）和 Histogram（下）
        ImGuiID dock_left_bottom_id = ImGui::DockBuilderSplitNode(
            dock_left_id,
            ImGuiDir_Down,      // 从下方切
            0.4f,               // 下方占 40%，上方占 60%
            nullptr,
            &dock_left_id
        );
        // 此时：
        // dock_left_id        = 左上（Controls）
        // dock_left_bottom_id = 左下（Histogram）
        // dock_main_id        = 中间（Image）

        // 把窗口 dock 到对应区域（名字要和 ImGui::Begin 的标题一致）
        ImGui::DockBuilderDockWindow("Controls",  dock_left_id);
        ImGui::DockBuilderDockWindow("Histogram", dock_left_bottom_id);
        ImGui::DockBuilderDockWindow("Image",     dock_main_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockFlags);

    ImGui::End();

    // ===== Controls 窗口 =====
    ImGui::Begin("Controls");

    // 路径输入
    ImGui::InputText("FITS Path", &_currentPath);

    // Browse 按钮单独一行
    if (ImGui::Button("Browse..."))
        open_file_dialog();

    // Bayer
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

    // 视图缩放
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

    // 白平衡
    bool wbChanged = false;
    if (ImGui::SliderFloat("R gain", &_wb.r, 0.1f, 4.0f)) wbChanged = true;
    if (ImGui::SliderFloat("G gain", &_wb.g, 0.1f, 4.0f)) wbChanged = true;
    if (ImGui::SliderFloat("B gain", &_wb.b, 0.1f, 4.0f)) wbChanged = true;

    // ★ 新增：自动白平衡按钮
    if (ImGui::Button("Auto White Balance"))
    {
        if (_hasImage)
        {
            if (_renderer.computeAutoWhiteBalance())
            {
                // 从 renderer 拿新的 wb
                kty::WhiteBalance newWb = _renderer.whiteBalance();
                _wb = newWb;

                // 自动白平衡会改变整体亮度分布，顺便重算 auto stretch + histogram
                if (_renderer.recomputeAutoStretch())
                {
                    _histogram.clear();
                    _renderer.getLumaHistogram(_histogram);
                }
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

    // ===== Image 窗口：ImGui::Image 显示预览纹理 + 右键平移 =====
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
                ImGuiIO& io = ImGui::GetIO();

                // 在 Image 窗口内按住右键拖动，改变视图平移
                if (ImGui::IsWindowHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
                {
                    ImVec2 d = io.MouseDelta;
                    // 把像素位移归一化到图像坐标（0~1），并且随缩放放大
                    float dx = -d.x / (float)texW;
                    float dy =  d.y / (float)texH;
                    _view.panX += dx * _view.scale;
                    _view.panY += dy * _view.scale;
                }

                // 更新核心视图参数
                _renderer.setViewParams(_view);

                // 让核心在 offscreen FBO 渲染一张预览纹理
                if (_renderer.renderPreview(texW, texH))
                {
                    unsigned int texId = _renderer.previewTextureId();
                    if (texId != 0)
                    {
                        ImGui::Image(
                            (ImTextureID)(intptr_t)texId,
                            ImVec2((float)texW, (float)texH),
                            ImVec2(0.0f, 1.0f),
                            ImVec2(1.0f, 0.0f)   // 反转 Y，保持上下正确
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


    // ===== 文件对话框 =====
    if (_showFileDialog)
        render_file_dialog();
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

bool ImguiApp::loadCurrentFits()
{
    if (_currentPath.empty())
        return false;

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

    return false;
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
                _selectedFileIndex = -1;   // ★ 重置选中
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

        // 显示标签：[D] 目录名 / 文件名
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
                    {
                        _showFileDialog = false;  // 成功加载后关闭对话框
                    }
                }
            }
        }
    }

    ImGui::EndChild();

    if (ImGui::Button("Open"))
    {
        if (!_currentPath.empty())
        {
            if (_renderer.loadFits(_currentPath, _bayer))
            {
                _hasImage = true;
                _renderer.setStretchParams(_stretch);
                _renderer.setWhiteBalance(_wb);
                _renderer.recomputeAutoStretch();
                _histogram.clear();
                _renderer.getLumaHistogram(_histogram);
            }
            _showFileDialog = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        _showFileDialog = false;

    ImGui::End();
}
