#pragma once
#include <string>
#include <vector>

#include "kty/FitsRenderer.h"
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

    bool loadCurrentFits();

private:
    GLFWwindow* _window = nullptr;

    kty::FitsRenderer _renderer;

    std::string _currentPath;
    std::string _fileDialogDir;
    std::vector<std::string> _fileEntries;
    int _selectedFileIndex = -1;
    bool _fileListDirty  = true;
    bool _showFileDialog = false;

    bool  _hasImage = false;

    // UI 状态
    kty::BayerPattern  _bayer      = kty::BayerPattern::RGGB;
    kty::StretchParams _stretch;
    kty::WhiteBalance  _wb;
    kty::ViewParams    _view;

    std::vector<float> _histogram;

    // 导出提示
    bool        _exportJustSucceeded = false;
    std::string _lastExportPath;
};
