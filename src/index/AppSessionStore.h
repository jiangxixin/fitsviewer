#pragma once

#include <string>
#include <vector>

namespace kty {

struct AppSessionStackFile {
    std::string path;
    int type = 0;
};

struct AppSessionData {
    std::string currentDir;
    std::string currentPath;
    int bayer = 1;

    bool useBias = true;
    bool useDark = true;
    bool optimizeDark = true;
    bool useFlat = true;
    bool removeHotPixels = true;
    bool removeLineDefects = true;
    int backgroundCalibrationMode = 1;
    bool qualityWeighting = true;
    int frameSelectionMode = 1;
    int autoQualityProfile = 1;
    float keepBestPercent = 100.0f;

    int rejectMethod = 2;
    float sigmaLow = 3.0f;
    float sigmaHigh = 3.0f;
    int minSamples = 3;
    int rejectIterations = 5;

    int denoisePreviewMode = 1;
    int denoiseExportMode = 1;
    float denoiseStrength = 0.35f;
    float backgroundSigma = 3.0f;
    int denoiseIterations = 1;

    std::vector<AppSessionStackFile> stackFiles;
};

class AppSessionStore
{
public:
    AppSessionStore() = default;
    ~AppSessionStore() = default;

    bool load(const std::string& dbPath, AppSessionData& outData, std::string& errorMessage) const;
    bool save(const std::string& dbPath, const AppSessionData& data, std::string& errorMessage) const;

private:
    bool initSchema(void* db, std::string& errorMessage) const;
};

} // namespace kty
