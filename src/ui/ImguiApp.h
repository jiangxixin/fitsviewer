#pragma once
#include <string>
#include <vector>

#include "kty/FitsRenderer.h"
#include "kty/StackCore.h"
#include <imgui.h>

struct GLFWwindow;

class ImguiApp
{
public:
    ImguiApp();
    ~ImguiApp();

    bool init();
    void run();
    void shutdown();

private:
    void frame();
    void render_ui();
    void render_file_dialog();

    void open_file_dialog();
    void refresh_file_list();

    // 根据 _currentPath 加载 FITS 并更新 renderer 状态
    bool loadCurrentFits();

    // 独立 Stack 窗口 UI
    void render_stack_window();

    // 根据 UI 中的 stack 列表重建 StackCore 内部状态
    void rebuildStackCoreFromUi();

private:
    GLFWwindow* _window = nullptr;

    kty::FitsRenderer _renderer;
    kty::StackCore    _stack;

    std::string _currentPath;
    std::string _fileDialogDir;
    std::vector<std::string> _fileEntries;
    bool _fileListDirty      = true;
    bool _showFileDialog     = false;
    int  _selectedFileIndex  = -1;

    bool  _hasImage = false;

    kty::BayerPattern  _bayer      = kty::BayerPattern::RGGB;
    kty::StretchParams _stretch;
    kty::WhiteBalance  _wb;
    kty::ViewParams    _view;

    std::vector<float> _histogram;

    bool        _exportJustSucceeded = false;
    std::string _lastExportPath;

    // ===== StackCore 相关 UI 状态 =====
    struct StackFileItem {
        std::string    path;
        kty::FrameType type;
    };
    std::vector<StackFileItem> _stackFiles;
    int _stackSelectedIndex = -1;

    int         _stackLightCount = 0;
    int         _stackDarkCount  = 0;
    int         _stackFlatCount  = 0;
    int         _stackBiasCount  = 0;

    std::string _stackLog;
};
