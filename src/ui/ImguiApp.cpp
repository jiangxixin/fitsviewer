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
#include <sstream>
#include <utility>
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

static bool path_exists_utf8(const std::string& path)
{
    if (path.empty())
        return false;
    try
    {
        return fs::exists(utf8_to_path(path));
    }
    catch (...)
    {
        return false;
    }
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
        _indexDbPath   = path_to_utf8(fs::current_path() / fs::u8path("fits_index.sqlite"));
    }
    catch (...)
    {
        _currentDir    = ".";
        _fileDialogDir = ".";
        _indexDbPath   = "fits_index.sqlite";
    }
    _fileListDirty = true;

    std::string indexError;
    if (!_fitsIndex.open(_indexDbPath, &indexError))
        _indexStatus = "Index DB open failed: " + indexError;
    else
        _indexStatus = "Index DB: " + _indexDbPath;

    loadSessionState();

    // First scan of current folder / restored folder
    refreshDirFits();
    if (!_currentPath.empty() && path_exists_utf8(_currentPath))
        setCurrentFitsPath(_currentPath);
    else
        loadDirFitsCurrent();
    refreshSearchResults();

    return true;
}

void ImguiApp::shutdown()
{
    if (_indexJob.worker.joinable())
        _indexJob.worker.join();
    if (_stackJob.worker.joinable())
        _stackJob.worker.join();

    saveSessionState();
    _renderer.shutdown();
    _fitsIndex.close();

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
    pollBackgroundJobs();

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
        _showingStackResult = false;
        _histogram.clear();
        return false;
    }

    if (_renderer.loadFits(_currentPath, _bayer))
    {
        _hasImage = true;
        _showingStackResult = false;
        _renderer.setStretchParams(_stretch);
        _renderer.setWhiteBalance(_wb);
        syncRendererLinearDenoise();

        refreshRendererHistogram();

        return true;
    }

    _hasImage = false;
    _showingStackResult = false;
    _histogram.clear();
    return false;
}

void ImguiApp::clearCurrentFitsSelection()
{
    _currentPath.clear();
    _hasImage = false;
    _showingStackResult = false;
    _histogram.clear();
}

bool ImguiApp::setCurrentFitsPath(const std::string& path)
{
    _currentPath = path;
    if (loadCurrentFits())
        return true;

    clearCurrentFitsSelection();
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
        else
            clearCurrentFitsSelection();
    }
    catch (const std::exception& e)
    {
        std::cerr << "refreshDirFits error: " << e.what() << "\n";
        clearCurrentFitsSelection();
    }
}

bool ImguiApp::loadDirFitsCurrent()
{
    if (_dirFitsIndex < 0 || _dirFitsIndex >= (int)_dirFits.size())
    {
        clearCurrentFitsSelection();
        return false;
    }

    return setCurrentFitsPath(_dirFits[_dirFitsIndex]);
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

void ImguiApp::refreshSearchResults()
{
    _searchResults.clear();
    _searchResultChecked.clear();
    _searchSelectedIndex = -1;

    if (!_fitsIndex.isOpen() || _currentDir.empty())
        return;

    if (_searchQuery.empty())
        return;

    std::string errorMessage;
    if (!_fitsIndex.search(_currentDir, _searchQuery, _searchResults, errorMessage))
    {
        _indexStatus = "Search failed: " + errorMessage;
        return;
    }

    _searchResultChecked.assign(_searchResults.size(), false);
}

void ImguiApp::syncRendererLinearDenoise()
{
    const kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    const bool previewEnabled = _showingStackResult &&
        denoiseCfg.previewMode == kty::DenoiseConfig::PreviewMode::FastBilateral;
    if (!previewEnabled)
    {
        _renderer.setLinearDenoiseConfig(false, 0.0f, denoiseCfg.backgroundSigma, 1);
    }
    else
    {
        _renderer.setLinearDenoiseConfig(true,
                                         denoiseCfg.strength,
                                         denoiseCfg.backgroundSigma,
                                         denoiseCfg.iterations);
    }

    const int exportMode = _showingStackResult ? static_cast<int>(denoiseCfg.exportMode) : 0;
    _renderer.setExportDenoiseConfig(exportMode,
                                     denoiseCfg.strength,
                                     denoiseCfg.backgroundSigma,
                                     denoiseCfg.iterations);
}

void ImguiApp::refreshRendererHistogram()
{
    if (!_hasImage)
    {
        _histogram.clear();
        return;
    }

    _renderer.recomputeAutoStretch();
    _histogram.clear();
    _renderer.getLumaHistogram(_histogram);
}

void ImguiApp::resetControlParams()
{
    _bayer = kty::BayerPattern::RGGB;
    _stretch = kty::StretchParams{};
    _wb = kty::WhiteBalance{};
    _view = kty::ViewParams{};

    if (!_hasImage)
    {
        _histogram.clear();
        return;
    }

    _renderer.setBayerPattern(_bayer);
    _renderer.setStretchParams(_stretch);
    _renderer.setWhiteBalance(_wb);
    _renderer.setViewParams(_view);
    refreshRendererHistogram();
}

void ImguiApp::resetStackParams()
{
    _stack.setCalibConfig(kty::CalibConfig{});
    _stack.setRejectConfig(kty::RejectConfig{});
    _stack.setDenoiseConfig(kty::DenoiseConfig{});

    if (_hasImage)
    {
        syncRendererLinearDenoise();
        refreshRendererHistogram();
    }

    _stackLog += "Stack parameters reset to defaults.\n";
}

void ImguiApp::indexCurrentFolder()
{
    startIndexJob();
}

void ImguiApp::startIndexJob()
{
    if (_currentDir.empty())
    {
        _indexStatus = "No folder selected.";
        return;
    }
    if (_indexJob.running)
        return;

    if (_indexJob.worker.joinable())
        _indexJob.worker.join();

    const std::string rootDir = _currentDir;
    const std::string dbPath = _indexDbPath;

    {
        std::lock_guard<std::mutex> lock(_indexJob.mutex);
        _indexJob.running = true;
        _indexJob.finished = false;
        _indexJob.success = false;
        _indexJob.progress = 0.0f;
        _indexJob.message = "Starting index job...";
        _indexJob.log.clear();
    }

    _indexJob.worker = std::thread([this, rootDir, dbPath]() {
        kty::FitsIndexDb db;
        std::string errorMessage;
        bool ok = db.open(dbPath, &errorMessage);
        std::string log;

        if (ok)
        {
            ok = db.rebuildIndexForRoot(
                rootDir,
                log,
                [this](float progress, const std::string& message) {
                    std::lock_guard<std::mutex> lock(_indexJob.mutex);
                    _indexJob.progress = progress;
                    _indexJob.message = message;
                });
        }
        else
        {
            log = errorMessage;
        }

        std::lock_guard<std::mutex> lock(_indexJob.mutex);
        _indexJob.running = false;
        _indexJob.finished = true;
        _indexJob.success = ok;
        _indexJob.progress = ok ? 1.0f : _indexJob.progress;
        _indexJob.message = ok ? "Index complete." : "Index failed.";
        _indexJob.log = log;
    });
}

void ImguiApp::loadSearchResult(int index)
{
    if (index < 0 || index >= (int)_searchResults.size())
        return;

    setCurrentFitsPath(_searchResults[index].path);
}

bool ImguiApp::addStackFile(const std::string& path, kty::FrameType type)
{
    auto it = std::find_if(_stackFiles.begin(), _stackFiles.end(),
                           [&](const StackFileItem& item) {
                               return item.path == path && item.type == type;
                           });
    if (it != _stackFiles.end())
        return false;

    _stackFiles.push_back({path, type});
    return true;
}

void ImguiApp::addSelectedSearchResultsToStack(kty::FrameType type)
{
    int added = 0;
    for (size_t i = 0; i < _searchResults.size() && i < _searchResultChecked.size(); ++i)
    {
        if (!_searchResultChecked[i])
            continue;
        if (addStackFile(_searchResults[i].path, type))
            ++added;
    }

    if (added > 0)
    {
        rebuildStackCoreFromUi();
        std::ostringstream oss;
        oss << "Added " << added << " indexed result(s) to stack.\n";
        _stackLog += oss.str();
    }
}

int ImguiApp::selectedSearchResultCount() const
{
    int count = 0;
    for (bool checked : _searchResultChecked)
    {
        if (checked)
            ++count;
    }
    return count;
}

bool ImguiApp::saveSessionState()
{
    if (_indexDbPath.empty())
        return false;

    kty::AppSessionData data;
    data.currentDir = _currentDir;
    data.currentPath = path_exists_utf8(_currentPath) ? _currentPath : std::string{};
    data.bayer = static_cast<int>(_bayer);

    const kty::CalibConfig calibCfg = _stack.calibConfig();
    data.useBias = calibCfg.useBias;
    data.useDark = calibCfg.useDark;
    data.optimizeDark = calibCfg.optimizeDark;
    data.useFlat = calibCfg.useFlat;
    data.removeHotPixels = calibCfg.removeHotPixels;
    data.removeLineDefects = calibCfg.removeLineDefects;
    data.backgroundCalibrationMode = static_cast<int>(calibCfg.backgroundCalibration);
    data.qualityWeighting = calibCfg.qualityWeighting;
    data.frameSelectionMode = static_cast<int>(calibCfg.frameSelectionMode);
    data.autoQualityProfile = static_cast<int>(calibCfg.autoQualityProfile);
    data.keepBestPercent = calibCfg.keepBestPercent;

    const kty::RejectConfig rejectCfg = _stack.rejectConfig();
    data.rejectMethod = static_cast<int>(rejectCfg.method);
    data.sigmaLow = rejectCfg.sigmaLow;
    data.sigmaHigh = rejectCfg.sigmaHigh;
    data.minSamples = rejectCfg.minSamples;
    data.rejectIterations = rejectCfg.iterations;

    const kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    data.denoisePreviewMode = static_cast<int>(denoiseCfg.previewMode);
    data.denoiseExportMode = static_cast<int>(denoiseCfg.exportMode);
    data.denoiseStrength = denoiseCfg.strength;
    data.backgroundSigma = denoiseCfg.backgroundSigma;
    data.denoiseIterations = denoiseCfg.iterations;

    data.stackFiles.reserve(_stackFiles.size());
    for (const auto& item : _stackFiles)
    {
        if (!item.path.empty())
            data.stackFiles.push_back({item.path, static_cast<int>(item.type)});
    }

    std::string errorMessage;
    if (!_sessionStore.save(_indexDbPath, data, errorMessage))
    {
        _indexStatus = "Save session failed: " + errorMessage;
        return false;
    }

    _indexStatus = "Session saved.";
    return true;
}

void ImguiApp::loadSessionState()
{
    if (_indexDbPath.empty())
        return;

    kty::AppSessionData data;
    std::string errorMessage;
    if (!_sessionStore.load(_indexDbPath, data, errorMessage))
    {
        _indexStatus = "Load session failed: " + errorMessage;
        return;
    }

    if (!data.currentDir.empty() && path_exists_utf8(data.currentDir))
        _currentDir = data.currentDir;
    _fileDialogDir = _currentDir;

    _bayer = static_cast<kty::BayerPattern>(std::clamp(data.bayer, 0, 4));

    kty::CalibConfig calibCfg = _stack.calibConfig();
    calibCfg.useBias = data.useBias;
    calibCfg.useDark = data.useDark;
    calibCfg.optimizeDark = data.optimizeDark;
    calibCfg.useFlat = data.useFlat;
    calibCfg.removeHotPixels = data.removeHotPixels;
    calibCfg.removeLineDefects = data.removeLineDefects;
    calibCfg.backgroundCalibration = static_cast<kty::BackgroundCalibrationMode>(
        std::clamp(data.backgroundCalibrationMode, 0, 2));
    calibCfg.qualityWeighting = data.qualityWeighting;
    calibCfg.frameSelectionMode = static_cast<kty::FrameSelectionMode>(
        std::clamp(data.frameSelectionMode, 0, 1));
    calibCfg.autoQualityProfile = static_cast<kty::AutoQualityProfile>(
        std::clamp(data.autoQualityProfile, 0, 2));
    calibCfg.keepBestPercent = std::clamp(data.keepBestPercent, 5.0f, 100.0f);
    _stack.setCalibConfig(calibCfg);

    kty::RejectConfig rejectCfg = _stack.rejectConfig();
    rejectCfg.method = static_cast<kty::RejectConfig::Method>(std::clamp(data.rejectMethod, 0, 2));
    rejectCfg.sigmaLow = data.sigmaLow;
    rejectCfg.sigmaHigh = data.sigmaHigh;
    rejectCfg.minSamples = data.minSamples;
    rejectCfg.iterations = std::clamp(data.rejectIterations, 1, 8);
    _stack.setRejectConfig(rejectCfg);

    kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    denoiseCfg.previewMode = static_cast<kty::DenoiseConfig::PreviewMode>(
        std::clamp(data.denoisePreviewMode, 0, 1));
    denoiseCfg.exportMode = static_cast<kty::DenoiseConfig::ExportMode>(
        std::clamp(data.denoiseExportMode, 0, 2));
    denoiseCfg.strength = data.denoiseStrength;
    denoiseCfg.backgroundSigma = data.backgroundSigma;
    denoiseCfg.iterations = data.denoiseIterations;
    _stack.setDenoiseConfig(denoiseCfg);

    _stackFiles.clear();
    for (const auto& item : data.stackFiles)
    {
        if (!path_exists_utf8(item.path))
            continue;

        kty::FrameType type = kty::FrameType::Light;
        if (item.type >= 0 && item.type <= 3)
            type = static_cast<kty::FrameType>(item.type);
        _stackFiles.push_back({item.path, type});
    }
    _stackSelectedIndex = _stackFiles.empty() ? -1 : 0;
    rebuildStackCoreFromUi();

    if (!data.currentPath.empty() && path_exists_utf8(data.currentPath))
        _currentPath = data.currentPath;
}

void ImguiApp::startStackJob()
{
    if (_stackJob.running)
        return;
    if (_stackFiles.empty())
    {
        _stackLog = "No frames added yet.\n";
        return;
    }
    if (_stackJob.worker.joinable())
        _stackJob.worker.join();

    const std::vector<StackFileItem> stackFiles = _stackFiles;
    const kty::CalibConfig calibCfg = _stack.calibConfig();
    const kty::RejectConfig rejectCfg = _stack.rejectConfig();
    const kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    const ::BayerPattern bayerPattern = static_cast<::BayerPattern>(_bayer);

    {
        std::lock_guard<std::mutex> lock(_stackJob.mutex);
        _stackJob.running = true;
        _stackJob.finished = false;
        _stackJob.success = false;
        _stackJob.progress = 0.0f;
        _stackJob.message = "Preparing stack...";
        _stackJob.log.clear();
        _stackJob.result = kty::StackResult{};
    }

    _stackJob.worker = std::thread([this, stackFiles, calibCfg, rejectCfg, denoiseCfg, bayerPattern]() {
        kty::StackCore workerStack;
        workerStack.setCalibConfig(calibCfg);
        workerStack.setRejectConfig(rejectCfg);
        workerStack.setDenoiseConfig(denoiseCfg);
        workerStack.setBayerPattern(bayerPattern);
        for (const auto& item : stackFiles)
            workerStack.addFrame(item.path, item.type);

        kty::StackResult result;
        const bool ok = workerStack.runStack(
            result,
            [this](float progress, const std::string& message) {
                std::lock_guard<std::mutex> lock(_stackJob.mutex);
                _stackJob.progress = progress;
                _stackJob.message = message;
            });

        std::lock_guard<std::mutex> lock(_stackJob.mutex);
        _stackJob.running = false;
        _stackJob.finished = true;
        _stackJob.success = ok;
        _stackJob.progress = ok ? 1.0f : _stackJob.progress;
        _stackJob.message = ok ? "Stack complete." : "Stack failed.";
        _stackJob.log = result.log.empty() ? workerStack.log() : result.log;
        _stackJob.result = std::move(result);
    });
}

void ImguiApp::pollBackgroundJobs()
{
    bool indexFinished = false;
    bool indexSuccess = false;
    std::string indexLog;
    {
        std::lock_guard<std::mutex> lock(_indexJob.mutex);
        if (_indexJob.finished)
        {
            indexFinished = true;
            indexSuccess = _indexJob.success;
            indexLog = _indexJob.log;
            _indexJob.finished = false;
        }
    }
    if (indexFinished)
    {
        if (_indexJob.worker.joinable())
            _indexJob.worker.join();
        _indexStatus = indexSuccess ? indexLog : ("Index failed: " + indexLog);
        if (indexSuccess)
            refreshSearchResults();
    }

    bool stackFinished = false;
    bool stackSuccess = false;
    std::string stackLog;
    kty::StackResult stackResult;
    {
        std::lock_guard<std::mutex> lock(_stackJob.mutex);
        if (_stackJob.finished)
        {
            stackFinished = true;
            stackSuccess = _stackJob.success;
            stackLog = _stackJob.log;
            stackResult = _stackJob.result;
            _stackJob.finished = false;
        }
    }
    if (stackFinished)
    {
        if (_stackJob.worker.joinable())
            _stackJob.worker.join();

        _stackLog = stackLog;
        if (stackSuccess)
        {
            _wb.r = 1.0f;
            _wb.g = 1.0f;
            _wb.b = 1.0f;
            if (_renderer.loadMonochromeImage(stackResult.finalImage.raw,
                                              stackResult.finalImage.width,
                                              stackResult.finalImage.height,
                                              static_cast<kty::BayerPattern>(stackResult.finalImage.bayer)))
            {
                _hasImage = true;
                _showingStackResult = true;
                _currentPath = "[Stack Result]";
                _view.scale = 1.0f;
                _view.panX = 0.0f;
                _view.panY = 0.0f;
                syncRendererLinearDenoise();
                _renderer.computeBackgroundNeutralization();
                _wb = _renderer.whiteBalance();
                _renderer.setStretchParams(_stretch);
                _renderer.setWhiteBalance(_wb);
                refreshRendererHistogram();
                _stackLog += "Stack preview updated with background neutralization.\n";
            }
            else
            {
                _stackLog += "Stack computed but preview upload failed.\n";
            }
        }
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

        // Left 25% → File Browse + Inspector
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

        // Middle split right 30% for Stack
        ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(
            dock_main_id,
            ImGuiDir_Right,
            0.30f,
            nullptr,
            &dock_main_id);

        // dock_left_id        -> File Browse
        // dock_left_bottom_id -> Inspector
        // dock_main_id        -> Image
        // dock_right_id       -> Stack

        ImGui::DockBuilderDockWindow("File Browse", dock_left_id);
        ImGui::DockBuilderDockWindow("Inspector",  dock_left_bottom_id);
        ImGui::DockBuilderDockWindow("Image",      dock_main_id);
        ImGui::DockBuilderDockWindow("Stack",      dock_right_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockFlags);

    ImGui::End();

    // ===== Windows =====

    // File browse (left, with file list + keyboard focus)
    render_file_browse_window();
    render_inspector_window();

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

void ImguiApp::render_inspector_window()
{
    if (!ImGui::Begin("Inspector"))
    {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("InspectorTabs"))
    {
        if (ImGui::BeginTabItem("Histogram"))
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
                                     ImVec2(0, 140));
            }
            else
            {
                ImGui::TextUnformatted("No histogram yet.");
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Histogram refreshes after drag release; preview stays GPU-live while adjusting.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls"))
        {
            render_controls_panel();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void ImguiApp::render_controls_panel()
{
    const double now = ImGui::GetTime();
    constexpr double kInteractiveAutoStretchIntervalSec = 1.0 / 24.0;

    ImGui::TextUnformatted("Image Controls");
    ImGui::SameLine();
    if (ImGui::Button("Reset Controls"))
        resetControlParams();

    bool refreshHistogram = false;

    ImGui::Separator();
    const char* patterns[] = {"None", "RGGB", "BGGR", "GRBG", "GBRG"};
    int bayerIndex = static_cast<int>(_bayer);
    if (ImGui::Combo("Bayer", &bayerIndex, patterns, IM_ARRAYSIZE(patterns)))
    {
        kty::BayerPattern newB = static_cast<kty::BayerPattern>(bayerIndex);
        if (newB != _bayer)
        {
            _bayer = newB;
            if (_hasImage)
            {
                _renderer.setBayerPattern(_bayer);
                refreshHistogram = true;
            }
        }
    }

    ImGui::Separator();
    const char* stretchModes[] = {"Linear", "Arcsinh", "Log", "Sqrt"};
    int stretchIndex = static_cast<int>(_stretch.mode);
    if (ImGui::Combo("Stretch mode", &stretchIndex, stretchModes, IM_ARRAYSIZE(stretchModes)))
    {
        _stretch.mode = static_cast<kty::StretchMode>(stretchIndex);
        if (_hasImage)
        {
            _renderer.setStretchParams(_stretch);
            if (_stretch.autoStretch)
                _renderer.recomputeAutoStretch();
            refreshHistogram = true;
        }
    }

    if (ImGui::Checkbox("Auto Stretch", &_stretch.autoStretch))
    {
        if (_hasImage)
        {
            _renderer.setStretchParams(_stretch);
            if (_stretch.autoStretch)
                _renderer.recomputeAutoStretch();
            refreshHistogram = true;
        }
    }

    bool stretchChanged = false;
    bool stretchCommitted = false;
    bool clipChanged = false;
    bool clipActive = false;

    clipChanged |= ImGui::SliderFloat("Black clip %", &_stretch.blackClip, 0.0f, 20.0f,
                                      "%.4f", ImGuiSliderFlags_AlwaysClamp);
    stretchChanged |= clipChanged;
    clipActive |= ImGui::IsItemActive();
    stretchCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    bool whiteClipChanged = ImGui::SliderFloat("White clip %", &_stretch.whiteClip, 0.0f, 20.0f,
                                               "%.4f", ImGuiSliderFlags_AlwaysClamp);
    clipChanged |= whiteClipChanged;
    stretchChanged |= whiteClipChanged;
    clipActive |= ImGui::IsItemActive();
    stretchCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    const bool strengthChanged = ImGui::SliderFloat("Stretch strength", &_stretch.strength, 1.0f, 20.0f,
                                                    "%.4f", ImGuiSliderFlags_AlwaysClamp);
    stretchChanged |= strengthChanged;
    stretchCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    if ((stretchChanged || stretchCommitted) && _hasImage)
    {
        _renderer.setStretchParams(_stretch);
        const bool shouldRecomputeAutoStretch = _stretch.autoStretch && clipChanged;
        if (shouldRecomputeAutoStretch)
        {
            const bool throttledInteractiveUpdate =
                clipActive &&
                (now - _lastInteractiveAutoStretchUpdate) >= kInteractiveAutoStretchIntervalSec;
            if (!clipActive || stretchCommitted || throttledInteractiveUpdate)
            {
                _renderer.recomputeAutoStretch();
                _lastInteractiveAutoStretchUpdate = now;
            }
        }
        if (stretchCommitted)
            refreshHistogram = true;
    }
    ImGui::Separator();
    const float zoomMin = 0.1f;
    const float zoomMax = 20.0f;
    if (ImGui::SliderFloat("Scale", &_view.scale, zoomMin, zoomMax, "%.4f",
                           ImGuiSliderFlags_Logarithmic))
    {
        _view.scale = std::clamp(_view.scale, zoomMin, zoomMax);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset View"))
    {
        _view.scale = 1.0f;
        _view.panX  = 0.0f;
        _view.panY  = 0.0f;
    }

    ImGui::Separator();
    bool wbChanged = false;
    bool wbCommitted = false;

    wbChanged |= ImGui::SliderFloat("R gain", &_wb.r, 0.1f, 4.0f,
                                    "%.4f", ImGuiSliderFlags_AlwaysClamp);
    wbCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    wbChanged |= ImGui::SliderFloat("G gain", &_wb.g, 0.1f, 4.0f,
                                    "%.4f", ImGuiSliderFlags_AlwaysClamp);
    wbCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    wbChanged |= ImGui::SliderFloat("B gain", &_wb.b, 0.1f, 4.0f,
                                    "%.4f", ImGuiSliderFlags_AlwaysClamp);
    wbCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    if ((wbChanged || wbCommitted) && _hasImage)
    {
        _renderer.setWhiteBalance(_wb);
        if (wbCommitted)
            refreshHistogram = true;
    }
    if (ImGui::Button("Auto White Balance"))
    {
        if (_hasImage && _renderer.computeAutoWhiteBalance())
        {
            _wb = _renderer.whiteBalance();
            refreshHistogram = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Background Neutralization"))
    {
        if (_hasImage && _renderer.computeBackgroundNeutralization())
        {
            _wb = _renderer.whiteBalance();
            refreshHistogram = true;
        }
    }

    if (refreshHistogram)
        refreshRendererHistogram();

    ImGui::Separator();
    ImGui::TextDisabled("Export PNG matches the current OpenGL render.");
    auto export_png = [&](bool hqExport) {
        _exportJustSucceeded = false;
        if (!_hasImage)
            return;

        fs::path inPath = utf8_to_path(_currentPath);
        fs::path outPath;
        if (!inPath.empty())
        {
            outPath = inPath;
            if (hqExport)
            {
                outPath.replace_extension();
                outPath += "_hq.png";
            }
            else
            {
                outPath.replace_extension(".png");
            }
        }
        else
        {
            outPath = fs::current_path() / fs::u8path(hqExport ? "output_hq.png" : "output.png");
        }

        std::vector<unsigned char> rgb;
        int w = 0, h = 0;
        const bool ok = hqExport ? _renderer.renderHqToImage(rgb, w, h)
                                 : _renderer.renderToImage(rgb, w, h);
        if (!ok)
            return;

        const int stride = w * 3;
        const std::string outPathUtf8 = path_to_utf8(outPath);
        if (stbi_write_png(outPathUtf8.c_str(), w, h, 3, rgb.data(), stride))
        {
            _lastExportPath = outPathUtf8;
            _exportJustSucceeded = true;
            std::cout << "PNG saved to " << _lastExportPath << "\n";
        }
        else
        {
            std::cerr << "Failed to write png: " << outPath << "\n";
        }
    };

    if (ImGui::Button("Export PNG"))
        export_png(false);

    const kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    if (denoiseCfg.exportMode != kty::DenoiseConfig::ExportMode::Off)
    {
        ImGui::SameLine();
        if (ImGui::Button("Export HQ PNG"))
            export_png(true);
    }

    if (_exportJustSucceeded && !_lastExportPath.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                           "Exported: %s", _lastExportPath.c_str());
    }
}

void ImguiApp::render_file_browse_window()
{
    ImGui::Begin("File Browse");

    ImGui::TextUnformatted("Folder, Index & Search");

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
    ImGui::SameLine();
    ImGui::BeginDisabled(_indexJob.running || _currentDir.empty());
    if (ImGui::Button("Index Folder"))
        indexCurrentFolder();
    ImGui::EndDisabled();

    if (!_indexStatus.empty())
        ImGui::TextWrapped("%s", _indexStatus.c_str());

    if (_indexJob.running)
    {
        float progress = 0.0f;
        std::string message;
        {
            std::lock_guard<std::mutex> lock(_indexJob.mutex);
            progress = _indexJob.progress;
            message = _indexJob.message;
        }
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), message.c_str());
    }

    bool searchChanged = ImGui::InputTextWithHint("Search Indexed FITS",
                                                  "filename, OBJECT, FILTER, EXTNAME...",
                                                  &_searchQuery);
    if (searchChanged)
        refreshSearchResults();

    ImGui::Separator();
    if (_searchQuery.empty())
    {
        ImGui::Text("FITS files in current folder: %d", (int)_dirFits.size());

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
    }
    else
    {
        ImGui::Text("Indexed matches: %d", (int)_searchResults.size());
        ImGui::SameLine();
        ImGui::TextDisabled("Selected: %d", selectedSearchResultCount());

        if (!_searchResults.empty())
        {
            if (ImGui::Button("Load Selected Result"))
                loadSearchResult(_searchSelectedIndex);
            ImGui::SameLine();
            if (ImGui::Button("Select All"))
            {
                for (size_t i = 0; i < _searchResultChecked.size(); ++i)
                    _searchResultChecked[i] = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Selection"))
            {
                for (size_t i = 0; i < _searchResultChecked.size(); ++i)
                    _searchResultChecked[i] = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Selected as Light"))
                addSelectedSearchResultsToStack(kty::FrameType::Light);
            ImGui::SameLine();
            if (ImGui::Button("Add Selected as Dark"))
                addSelectedSearchResultsToStack(kty::FrameType::Dark);

            if (ImGui::Button("Add Selected as Flat"))
                addSelectedSearchResultsToStack(kty::FrameType::Flat);
            ImGui::SameLine();
            if (ImGui::Button("Add Selected as Bias"))
                addSelectedSearchResultsToStack(kty::FrameType::Bias);
        }

        ImGui::BeginChild("search_result_list", ImVec2(0, 0), true);
        for (int i = 0; i < (int)_searchResults.size(); ++i)
        {
            const auto& item = _searchResults[i];
            const bool selected = (i == _searchSelectedIndex);
            bool checked = (i < (int)_searchResultChecked.size()) ? _searchResultChecked[i] : false;

            ImGui::PushID(i);
            if (ImGui::Checkbox("##checked", &checked) && i < (int)_searchResultChecked.size())
                _searchResultChecked[i] = checked;
            ImGui::SameLine();

            std::ostringstream label;
            label << item.fileName;
            if (item.width > 0 && item.height > 0)
                label << "  [" << item.width << "x" << item.height;
            if (item.depth > 1)
                label << "x" << item.depth;
            if (item.width > 0 && item.height > 0)
                label << "]";
            if (item.imageHduCount > 0)
                label << "  IMG:" << item.imageHduCount;
            if (item.hduCount > 0)
                label << "  HDU:" << item.hduCount;

            if (ImGui::Selectable(label.str().c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                _searchSelectedIndex = i;
                if (ImGui::IsMouseDoubleClicked(0))
                    loadSearchResult(i);
            }

            ImGui::TextDisabled("%s", item.path.c_str());
            if (!item.objectName.empty())
                ImGui::Text("OBJECT: %s", item.objectName.c_str());
            if (!item.filterName.empty() || !item.extNames.empty() || !item.dateObs.empty())
            {
                std::ostringstream meta;
                if (!item.filterName.empty())
                    meta << "FILTER=" << item.filterName;
                if (!item.dateObs.empty())
                {
                    if (!meta.str().empty())
                        meta << "  ";
                    meta << "DATE-OBS=" << item.dateObs;
                }
                if (!item.extNames.empty())
                {
                    const std::string current = meta.str();
                    if (!current.empty())
                        meta << "  ";
                    meta << "EXT=" << item.extNames;
                }
                const std::string metaText = meta.str();
                ImGui::TextWrapped("%s", metaText.c_str());
            }
            ImGui::Separator();
            ImGui::PopID();
        }

        if (_searchResults.empty())
            ImGui::TextDisabled("No indexed result. Click 'Index Folder' first or adjust keywords.");

        ImGui::EndChild();
    }

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
            if (addStackFile(_currentPath, kty::FrameType::Light))
                rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Dark"))
        {
            if (addStackFile(_currentPath, kty::FrameType::Dark))
                rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Flat"))
        {
            if (addStackFile(_currentPath, kty::FrameType::Flat))
                rebuildStackCoreFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add as Bias"))
        {
            if (addStackFile(_currentPath, kty::FrameType::Bias))
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
    ImGui::SameLine();
    if (ImGui::Button("Save Stack Session"))
    {
        if (saveSessionState())
            _stackLog += "Stack session saved.\n";
        else
            _stackLog += "Stack session save failed.\n";
    }

    ImGui::Separator();

    // Calibration config
    kty::CalibConfig calibCfg = _stack.calibConfig();
    ImGui::Checkbox("Use Bias", &calibCfg.useBias);
    ImGui::Checkbox("Use Dark", &calibCfg.useDark);
    ImGui::Checkbox("Dark Optimization", &calibCfg.optimizeDark);
    ImGui::Checkbox("Use Flat", &calibCfg.useFlat);
    ImGui::Checkbox("Hot Pixel Correction", &calibCfg.removeHotPixels);
    ImGui::Checkbox("Bad Column/Row Correction", &calibCfg.removeLineDefects);
    const char* backgroundModes[] = {"Off", "Per Channel", "RGB Channels"};
    int backgroundMode = static_cast<int>(calibCfg.backgroundCalibration);
    if (ImGui::Combo("Background Calibration", &backgroundMode, backgroundModes, IM_ARRAYSIZE(backgroundModes)))
        calibCfg.backgroundCalibration = static_cast<kty::BackgroundCalibrationMode>(backgroundMode);
    ImGui::Checkbox("Quality Weighting", &calibCfg.qualityWeighting);
    const char* selectionModes[] = {"Manual Keep %", "Auto Star Quality"};
    int frameSelectionMode = static_cast<int>(calibCfg.frameSelectionMode);
    if (ImGui::Combo("Frame Selection", &frameSelectionMode, selectionModes, IM_ARRAYSIZE(selectionModes)))
        calibCfg.frameSelectionMode = static_cast<kty::FrameSelectionMode>(frameSelectionMode);
    if (calibCfg.frameSelectionMode == kty::FrameSelectionMode::AutoStarQuality)
    {
        const char* qualityProfiles[] = {"Conservative", "Balanced", "Aggressive"};
        int autoQualityProfile = static_cast<int>(calibCfg.autoQualityProfile);
        if (ImGui::Combo("Selection Profile", &autoQualityProfile, qualityProfiles, IM_ARRAYSIZE(qualityProfiles)))
            calibCfg.autoQualityProfile = static_cast<kty::AutoQualityProfile>(autoQualityProfile);
        ImGui::TextDisabled("Uses star count, FWHM, roundness and SNR to choose a keep ratio.");
    }
    else
    {
        ImGui::SliderFloat("Keep Best %", &calibCfg.keepBestPercent, 5.0f, 100.0f, "%.1f");
    }
    _stack.setCalibConfig(calibCfg);

    ImGui::Separator();
    kty::RejectConfig rejectCfg = _stack.rejectConfig();
    const char* rejectMethods[] = {"None", "Sigma Clip", "Auto Adaptive Weighted Average"};
    int rejectMethod = static_cast<int>(rejectCfg.method);
    if (ImGui::Combo("Rejection", &rejectMethod, rejectMethods, IM_ARRAYSIZE(rejectMethods)))
        rejectCfg.method = static_cast<kty::RejectConfig::Method>(rejectMethod);
    if (rejectCfg.method == kty::RejectConfig::Method::SigmaClip)
    {
        ImGui::SliderFloat("Low Sigma", &rejectCfg.sigmaLow, 1.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("High Sigma", &rejectCfg.sigmaHigh, 1.5f, 5.0f, "%.2f");
        ImGui::SliderInt("Min Samples", &rejectCfg.minSamples, 3, 20);
        ImGui::SliderInt("Iterations", &rejectCfg.iterations, 1, 8);
    }
    else if (rejectCfg.method == kty::RejectConfig::Method::AutoAdaptiveWeightedAverage)
    {
        ImGui::SliderInt("Min Samples", &rejectCfg.minSamples, 3, 20);
        ImGui::SliderInt("Iterations", &rejectCfg.iterations, 1, 8);
        ImGui::TextDisabled("DSS-style: soft down-weighting instead of hard rejection.");
    }
    _stack.setRejectConfig(rejectCfg);

    ImGui::Separator();
    kty::DenoiseConfig denoiseCfg = _stack.denoiseConfig();
    const char* previewModes[] = {"Off", "Fast Bilateral Preview"};
    const char* exportModes[] = {"Off", "HQ Wavelet Export", "HQ Wavelet + BM3D-style Export"};
    int previewMode = static_cast<int>(denoiseCfg.previewMode);
    int exportMode = static_cast<int>(denoiseCfg.exportMode);
    bool denoiseChanged = ImGui::Combo("Preview Denoise", &previewMode, previewModes, IM_ARRAYSIZE(previewModes));
    denoiseChanged |= ImGui::Combo("Export Denoise", &exportMode, exportModes, IM_ARRAYSIZE(exportModes));
    denoiseCfg.previewMode = static_cast<kty::DenoiseConfig::PreviewMode>(previewMode);
    denoiseCfg.exportMode = static_cast<kty::DenoiseConfig::ExportMode>(exportMode);
    if (denoiseCfg.previewMode != kty::DenoiseConfig::PreviewMode::Off ||
        denoiseCfg.exportMode != kty::DenoiseConfig::ExportMode::Off)
    {
        denoiseChanged |= ImGui::SliderFloat("Denoise Strength", &denoiseCfg.strength, 0.0f, 1.0f, "%.2f");
        denoiseChanged |= ImGui::SliderFloat("Background Sigma", &denoiseCfg.backgroundSigma, 1.0f, 6.0f, "%.2f");
        denoiseChanged |= ImGui::SliderInt("Denoise Iterations", &denoiseCfg.iterations, 1, 4);
    }
    _stack.setDenoiseConfig(denoiseCfg);
    if (denoiseChanged && _hasImage)
    {
        syncRendererLinearDenoise();
        refreshRendererHistogram();
    }

    ImGui::Separator();

    // Run / reset stack
    ImGui::BeginDisabled(_stackJob.running);
    if (ImGui::Button("Run Stack"))
    {
        _stackLog.clear();
        startStackJob();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Reset Stack Params"))
        resetStackParams();

    ImGui::SameLine();
    if (ImGui::Button("Reset Stack Project"))
    {
        _stackFiles.clear();
        _stack.reset();
        _stackLightCount = _stackDarkCount = _stackFlatCount = _stackBiasCount = 0;
        _stackSelectedIndex = -1;
        _stackLog.clear();
    }

    if (_stackJob.running)
    {
        float progress = 0.0f;
        std::string message;
        {
            std::lock_guard<std::mutex> lock(_stackJob.mutex);
            progress = _stackJob.progress;
            message = _stackJob.message;
        }
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), message.c_str());
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
        refreshSearchResults();
        _showFileDialog = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        _showFileDialog = false;

    ImGui::End();
}
