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
    bool useFlat = true;

    int rejectMethod = 1;
    float sigmaLow = 3.0f;
    float sigmaHigh = 3.0f;
    int minSamples = 3;

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
