#pragma once

#include <functional>
#include <string>
#include <vector>

namespace kty {

struct FitsHduSummary {
    int hduIndex = 0;
    int hduType = 0;
    int bitpix = 0;
    int width = 0;
    int height = 0;
    int depth = 1;
    std::string hduTypeName;
    std::string extName;
    std::string objectName;
    std::string filterName;
    std::string dateObs;
};

struct FitsFileSummary {
    std::string path;
    std::string rootDir;
    std::string fileName;
    std::string directory;
    long long fileSize = 0;
    long long modifiedUnix = 0;
    int hduCount = 0;
    int imageHduCount = 0;
    int width = 0;
    int height = 0;
    int depth = 1;
    int bitpix = 0;
    std::string objectName;
    std::string filterName;
    std::string dateObs;
    std::string extNames;
    std::string normalizedSearchText;
    std::vector<FitsHduSummary> hdus;
};

struct FitsSearchResult {
    std::string path;
    std::string fileName;
    std::string directory;
    std::string rootDir;
    int hduCount = 0;
    int imageHduCount = 0;
    int width = 0;
    int height = 0;
    int depth = 1;
    std::string objectName;
    std::string filterName;
    std::string dateObs;
    std::string extNames;
};

class FitsIndexDb
{
public:
    using ProgressCallback = std::function<void(float, const std::string&)>;

    FitsIndexDb();
    ~FitsIndexDb();

    bool open(const std::string& dbPath, std::string* errorMessage = nullptr);
    void close();

    bool isOpen() const { return _db != nullptr; }
    const std::string& dbPath() const { return _dbPath; }

    bool rebuildIndexForRoot(const std::string& rootDir,
                            std::string& log,
                            const ProgressCallback& progressCallback = {});
    bool search(const std::string& rootDir,
                const std::string& query,
                std::vector<FitsSearchResult>& outResults,
                std::string& errorMessage,
                int limit = 200) const;

private:
    void* _db = nullptr; // sqlite3*
    std::string _dbPath;

private:
    bool initSchema(std::string* errorMessage);
    bool deleteRootRows(const std::string& rootDir, std::string& errorMessage);
    bool upsertFileSummary(const FitsFileSummary& summary, std::string& errorMessage);
};

bool read_fits_summary(const std::string& path, const std::string& rootDir, FitsFileSummary& outSummary);

} // namespace kty
