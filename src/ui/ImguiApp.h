#pragma once
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kty/FitsRenderer.h"
#include "kty/StackCore.h"
#include "AppSessionStore.h"
#include "FitsIndex.h"
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

    void open_file_dialog();     // Open "select folder" dialog
    void refresh_file_list();    // Entries in file dialog (directories + files)

    // Load FITS from _currentPath and update renderer
    bool loadCurrentFits();

    // Individual windows
    void render_stack_window();
    void render_file_browse_window();
    void render_inspector_window();
    void render_controls_panel();

    // Rebuild StackCore from UI list
    void rebuildStackCoreFromUi();

    // Folder mode: scan current folder for FITS files
    void refreshDirFits();
    void clearCurrentFitsSelection();
    bool setCurrentFitsPath(const std::string& path);

    // Load FITS at current index in _dirFits
    bool loadDirFitsCurrent();

    // Browse FITS in folder (delta = -1 / +1)
    void browseDirFits(int delta);
    void refreshSearchResults();
    void syncRendererLinearDenoise();
    void refreshRendererHistogram();
    void resetControlParams();
    void resetStackParams();
    void indexCurrentFolder();
    void loadSearchResult(int index);
    bool addStackFile(const std::string& path, kty::FrameType type);
    void addSelectedSearchResultsToStack(kty::FrameType type);
    int selectedSearchResultCount() const;
    bool saveSessionState();
    void loadSessionState();
    void startIndexJob();
    void startStackJob();
    void pollBackgroundJobs();

private:
    struct IndexJobState {
        std::thread worker;
        std::mutex mutex;
        bool running = false;
        bool finished = false;
        bool success = false;
        float progress = 0.0f;
        std::string message;
        std::string log;
    };

    struct StackJobState {
        std::thread worker;
        std::mutex mutex;
        bool running = false;
        bool finished = false;
        bool success = false;
        float progress = 0.0f;
        std::string message;
        std::string log;
        kty::StackResult result;
    };

    GLFWwindow* _window = nullptr;

    kty::FitsRenderer _renderer;
    kty::StackCore    _stack;

    // Currently displayed FITS full path
    std::string _currentPath;

    // Current folder for browsing (File Browse window)
    std::string _currentDir;

    // FITS files in current directory (full paths)
    std::vector<std::string> _dirFits;
    int  _dirFitsIndex = -1;
    std::string _searchQuery;
    std::vector<kty::FitsSearchResult> _searchResults;
    std::vector<bool> _searchResultChecked;
    int _searchSelectedIndex = -1;

    // File dialog internal state
    std::string _fileDialogDir;
    std::vector<std::string> _fileEntries;
    bool _fileListDirty      = true;
    bool _showFileDialog     = false;
    int  _selectedFileIndex  = -1;

    bool  _hasImage = false;
    bool  _showingStackResult = false;

    kty::BayerPattern  _bayer      = kty::BayerPattern::RGGB;
    kty::StretchParams _stretch;
    kty::WhiteBalance  _wb;
    kty::ViewParams    _view;

    std::vector<float> _histogram;

    bool        _exportJustSucceeded = false;
    std::string _lastExportPath;

    kty::FitsIndexDb _fitsIndex;
    kty::AppSessionStore _sessionStore;
    std::string _indexDbPath;
    std::string _indexStatus;

    // ===== StackCore UI state =====
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
    IndexJobState _indexJob;
    StackJobState _stackJob;
    double _lastInteractiveAutoStretchUpdate = -1.0;
};
