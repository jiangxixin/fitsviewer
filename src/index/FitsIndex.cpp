#include "FitsIndex.h"

#include <fitsio.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace kty {

namespace {

static std::string path_to_utf8(const fs::path& p)
{
    return p.u8string();
}

static fs::path utf8_to_path(const std::string& s)
{
    return fs::u8path(s);
}

static bool is_fits_path(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".fits" || ext == ".fit" || ext == ".fts";
}

static std::string trim_copy(std::string value)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(),
                             [&](unsigned char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](unsigned char c) { return !is_space(c); }).base(),
                value.end());
    return value;
}

static std::string read_key_string(fitsfile* fptr, const char* key)
{
    char buffer[FLEN_VALUE] = {0};
    int status = 0;
    if (fits_read_key(fptr, TSTRING, const_cast<char*>(key), buffer, nullptr, &status))
        return {};
    return trim_copy(buffer);
}

static std::string hdu_type_name(int hduType)
{
    switch (hduType)
    {
    case IMAGE_HDU: return "IMAGE";
    case ASCII_TBL: return "ASCII_TBL";
    case BINARY_TBL: return "BINARY_TBL";
    default: return "UNKNOWN";
    }
}

static bool exec_sql(sqlite3* db, const char* sql, std::string& errorMessage)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK)
        return true;

    errorMessage = err ? err : sqlite3_errmsg(db);
    if (err)
        sqlite3_free(err);
    return false;
}

static std::vector<std::string> split_terms(const std::string& query)
{
    std::vector<std::string> terms;
    std::string current;
    for (unsigned char c : query)
    {
        if (!std::isalnum(c))
        {
            if (!current.empty())
            {
                terms.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(static_cast<char>(std::tolower(c)));
    }
    if (!current.empty())
        terms.push_back(current);
    return terms;
}

static std::string normalize_search_text(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

static std::string column_text(sqlite3_stmt* stmt, int column)
{
    const unsigned char* text = sqlite3_column_text(stmt, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

static bool table_has_column(sqlite3* db, const char* tableName, const char* columnName)
{
    std::string sql = "PRAGMA table_info(" + std::string(tableName) + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const std::string existing = column_text(stmt, 1);
        if (existing == columnName)
        {
            found = true;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

} // namespace

FitsIndexDb::FitsIndexDb() = default;

FitsIndexDb::~FitsIndexDb()
{
    close();
}

bool FitsIndexDb::open(const std::string& dbPath, std::string* errorMessage)
{
    close();

    sqlite3* db = nullptr;
    const int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc != SQLITE_OK)
    {
        if (errorMessage)
            *errorMessage = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db)
            sqlite3_close(db);
        return false;
    }

    _db = db;
    _dbPath = dbPath;

    std::string schemaError;
    if (!initSchema(&schemaError))
    {
        if (errorMessage)
            *errorMessage = schemaError;
        close();
        return false;
    }

    return true;
}

void FitsIndexDb::close()
{
    if (_db)
        sqlite3_close(static_cast<sqlite3*>(_db));
    _db = nullptr;
    _dbPath.clear();
}

bool FitsIndexDb::initSchema(std::string* errorMessage)
{
    sqlite3* db = static_cast<sqlite3*>(_db);
    if (!db)
    {
        if (errorMessage)
            *errorMessage = "database is not open";
        return false;
    }

    std::string execError;
    const char* sql = R"SQL(
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS fits_files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    root_dir TEXT NOT NULL,
    file_name TEXT NOT NULL,
    directory TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    modified_unix INTEGER NOT NULL,
    hdu_count INTEGER NOT NULL,
    image_hdu_count INTEGER NOT NULL,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL,
    depth INTEGER NOT NULL,
    bitpix INTEGER NOT NULL,
    object_name TEXT NOT NULL,
    filter_name TEXT NOT NULL,
    date_obs TEXT NOT NULL,
    ext_names TEXT NOT NULL,
    normalized_search_text TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS fits_hdus (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL,
    hdu_index INTEGER NOT NULL,
    hdu_type INTEGER NOT NULL,
    hdu_type_name TEXT NOT NULL,
    ext_name TEXT NOT NULL,
    object_name TEXT NOT NULL,
    filter_name TEXT NOT NULL,
    date_obs TEXT NOT NULL,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL,
    depth INTEGER NOT NULL,
    bitpix INTEGER NOT NULL,
    FOREIGN KEY(file_id) REFERENCES fits_files(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_fits_files_root_dir ON fits_files(root_dir);
CREATE INDEX IF NOT EXISTS idx_fits_files_file_name ON fits_files(file_name);
CREATE INDEX IF NOT EXISTS idx_fits_hdus_file_id ON fits_hdus(file_id);
)SQL";

    if (!exec_sql(db, sql, execError))
    {
        if (errorMessage)
            *errorMessage = execError;
        return false;
    }

    if (!table_has_column(db, "fits_files", "normalized_search_text"))
    {
        const char* alterSql =
            "ALTER TABLE fits_files ADD COLUMN normalized_search_text TEXT NOT NULL DEFAULT '';";
        if (!exec_sql(db, alterSql, execError))
        {
            if (errorMessage)
                *errorMessage = execError;
            return false;
        }
    }

    const char* indexSql =
        "CREATE INDEX IF NOT EXISTS idx_fits_files_normalized_search_text "
        "ON fits_files(normalized_search_text);";
    if (!exec_sql(db, indexSql, execError))
    {
        if (errorMessage)
            *errorMessage = execError;
        return false;
    }

    return true;
}

bool FitsIndexDb::deleteRootRows(const std::string& rootDir, std::string& errorMessage)
{
    sqlite3* db = static_cast<sqlite3*>(_db);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM fits_files WHERE root_dir = ?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, rootDir.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool FitsIndexDb::upsertFileSummary(const FitsFileSummary& summary, std::string& errorMessage)
{
    sqlite3* db = static_cast<sqlite3*>(_db);
    sqlite3_stmt* stmt = nullptr;
    const char* fileSql = R"SQL(
INSERT INTO fits_files (
    path, root_dir, file_name, directory, file_size, modified_unix,
    hdu_count, image_hdu_count, width, height, depth, bitpix,
    object_name, filter_name, date_obs, ext_names, normalized_search_text
) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17);
)SQL";

    if (sqlite3_prepare_v2(db, fileSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, summary.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, summary.rootDir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, summary.fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, summary.directory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, summary.fileSize);
    sqlite3_bind_int64(stmt, 6, summary.modifiedUnix);
    sqlite3_bind_int(stmt, 7, summary.hduCount);
    sqlite3_bind_int(stmt, 8, summary.imageHduCount);
    sqlite3_bind_int(stmt, 9, summary.width);
    sqlite3_bind_int(stmt, 10, summary.height);
    sqlite3_bind_int(stmt, 11, summary.depth);
    sqlite3_bind_int(stmt, 12, summary.bitpix);
    sqlite3_bind_text(stmt, 13, summary.objectName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, summary.filterName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, summary.dateObs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, summary.extNames.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, summary.normalizedSearchText.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);

    const sqlite3_int64 fileId = sqlite3_last_insert_rowid(db);

    const char* hduSql = R"SQL(
INSERT INTO fits_hdus (
    file_id, hdu_index, hdu_type, hdu_type_name, ext_name,
    object_name, filter_name, date_obs, width, height, depth, bitpix
) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12);
)SQL";
    if (sqlite3_prepare_v2(db, hduSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    for (const FitsHduSummary& hdu : summary.hdus)
    {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, fileId);
        sqlite3_bind_int(stmt, 2, hdu.hduIndex);
        sqlite3_bind_int(stmt, 3, hdu.hduType);
        sqlite3_bind_text(stmt, 4, hdu.hduTypeName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, hdu.extName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, hdu.objectName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, hdu.filterName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, hdu.dateObs.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 9, hdu.width);
        sqlite3_bind_int(stmt, 10, hdu.height);
        sqlite3_bind_int(stmt, 11, hdu.depth);
        sqlite3_bind_int(stmt, 12, hdu.bitpix);

        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            errorMessage = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return true;
}

bool FitsIndexDb::rebuildIndexForRoot(const std::string& rootDir,
                                      std::string& log,
                                      const ProgressCallback& progressCallback)
{
    sqlite3* db = static_cast<sqlite3*>(_db);
    if (!db)
    {
        log = "SQLite database is not open.";
        return false;
    }

    fs::path rootPath = utf8_to_path(rootDir);
    if (rootDir.empty() || !fs::exists(rootPath) || !fs::is_directory(rootPath))
    {
        log = "Index root is not a valid directory.";
        return false;
    }

    if (progressCallback)
        progressCallback(0.0f, "Scanning FITS files...");

    std::vector<fs::path> fitsPaths;
    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(rootPath))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path path = entry.path();
            const std::string name = path_to_utf8(path.filename());
            if (!name.empty() && name[0] == '.')
                continue;
            if (!is_fits_path(path))
                continue;
            fitsPaths.push_back(path);
        }
    }
    catch (const std::exception& e)
    {
        log = e.what();
        return false;
    }

    std::string errorMessage;
    if (!exec_sql(db, "BEGIN IMMEDIATE TRANSACTION;", errorMessage))
    {
        log = errorMessage;
        return false;
    }

    bool ok = deleteRootRows(rootDir, errorMessage);
    int indexedFiles = 0;
    int failedFiles = 0;

    if (ok)
    {
        try
        {
            const size_t total = fitsPaths.size();
            for (size_t i = 0; i < fitsPaths.size(); ++i)
            {
                const fs::path& path = fitsPaths[i];

                FitsFileSummary summary;
                if (!read_fits_summary(path_to_utf8(path), rootDir, summary))
                {
                    ++failedFiles;
                    if (progressCallback)
                    {
                        const float progress = total > 0 ? static_cast<float>(i + 1) / static_cast<float>(total) : 1.0f;
                        progressCallback(progress, "Reading FITS headers...");
                    }
                    continue;
                }

                if (!upsertFileSummary(summary, errorMessage))
                {
                    ok = false;
                    break;
                }
                ++indexedFiles;

                if (progressCallback)
                {
                    const float progress = total > 0 ? static_cast<float>(i + 1) / static_cast<float>(total) : 1.0f;
                    progressCallback(progress, "Writing index database...");
                }
            }
        }
        catch (const std::exception& e)
        {
            ok = false;
            errorMessage = e.what();
        }
    }

    std::string txError;
    if (ok)
        exec_sql(db, "COMMIT;", txError);
    else
        exec_sql(db, "ROLLBACK;", txError);

    if (!ok)
    {
        log = errorMessage.empty() ? "indexing failed" : errorMessage;
        return false;
    }

    if (progressCallback)
        progressCallback(1.0f, "Index complete.");

    std::ostringstream oss;
    oss << "Indexed " << indexedFiles << " FITS file(s)";
    if (failedFiles > 0)
        oss << ", skipped " << failedFiles << " unreadable file(s)";
    oss << ".";
    log = oss.str();
    return true;
}

bool FitsIndexDb::search(const std::string& rootDir,
                         const std::string& query,
                         std::vector<FitsSearchResult>& outResults,
                         std::string& errorMessage,
                         int limit) const
{
    outResults.clear();

    sqlite3* db = static_cast<sqlite3*>(_db);
    if (!db)
    {
        errorMessage = "SQLite database is not open.";
        return false;
    }

    std::vector<std::string> terms = split_terms(query);
    const std::string normalizedQuery = normalize_search_text(query);

    std::string sql =
        "SELECT path, file_name, directory, root_dir, hdu_count, image_hdu_count, width, height, depth, "
        "object_name, filter_name, date_obs, ext_names "
        "FROM fits_files WHERE root_dir = ?1";

    for (size_t i = 0; i < terms.size(); ++i)
    {
        const int bindIndex = static_cast<int>(i) + 2;
        sql += " AND (lower(path) LIKE ?" + std::to_string(bindIndex) +
               " OR lower(file_name) LIKE ?" + std::to_string(bindIndex) +
               " OR lower(object_name) LIKE ?" + std::to_string(bindIndex) +
               " OR lower(filter_name) LIKE ?" + std::to_string(bindIndex) +
               " OR lower(date_obs) LIKE ?" + std::to_string(bindIndex) +
               " OR lower(ext_names) LIKE ?" + std::to_string(bindIndex) + ")";
    }
    if (!normalizedQuery.empty())
    {
        const int bindIndex = static_cast<int>(terms.size()) + 2;
        sql += " AND normalized_search_text LIKE ?" + std::to_string(bindIndex);
    }
    sql += " ORDER BY file_name COLLATE NOCASE ASC LIMIT " + std::to_string(std::max(1, limit)) + ";";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, rootDir.c_str(), -1, SQLITE_TRANSIENT);
    for (size_t i = 0; i < terms.size(); ++i)
    {
        const std::string likeValue = "%" + terms[i] + "%";
        sqlite3_bind_text(stmt, static_cast<int>(i) + 2, likeValue.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!normalizedQuery.empty())
    {
        const std::string normalizedLikeValue = "%" + normalizedQuery + "%";
        sqlite3_bind_text(stmt, static_cast<int>(terms.size()) + 2,
                          normalizedLikeValue.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        FitsSearchResult item;
        item.path = column_text(stmt, 0);
        item.fileName = column_text(stmt, 1);
        item.directory = column_text(stmt, 2);
        item.rootDir = column_text(stmt, 3);
        item.hduCount = sqlite3_column_int(stmt, 4);
        item.imageHduCount = sqlite3_column_int(stmt, 5);
        item.width = sqlite3_column_int(stmt, 6);
        item.height = sqlite3_column_int(stmt, 7);
        item.depth = sqlite3_column_int(stmt, 8);
        item.objectName = column_text(stmt, 9);
        item.filterName = column_text(stmt, 10);
        item.dateObs = column_text(stmt, 11);
        item.extNames = column_text(stmt, 12);
        outResults.push_back(std::move(item));
    }

    sqlite3_finalize(stmt);
    return true;
}

bool read_fits_summary(const std::string& path, const std::string& rootDir, FitsFileSummary& outSummary)
{
    fitsfile* fptr = nullptr;
    int status = 0;

    if (fits_open_file(&fptr, path.c_str(), READONLY, &status))
        return false;

    outSummary = FitsFileSummary{};
    outSummary.path = path;
    outSummary.rootDir = rootDir;

    fs::path fsPath = utf8_to_path(path);
    outSummary.fileName = path_to_utf8(fsPath.filename());
    outSummary.directory = path_to_utf8(fsPath.parent_path());

    try
    {
        const auto fileSize = fs::file_size(fsPath);
        outSummary.fileSize = static_cast<long long>(fileSize);
        const auto writeTime = fs::last_write_time(fsPath).time_since_epoch();
        outSummary.modifiedUnix = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(writeTime).count());
    }
    catch (...)
    {
        outSummary.fileSize = 0;
        outSummary.modifiedUnix = 0;
    }

    int hdus = 0;
    if (fits_get_num_hdus(fptr, &hdus, &status))
    {
        fits_close_file(fptr, &status);
        return false;
    }
    outSummary.hduCount = hdus;

    std::set<std::string> extNames;
    bool firstImageCaptured = false;

    for (int hduIndex = 1; hduIndex <= hdus; ++hduIndex)
    {
        int hduType = 0;
        status = 0;
        if (fits_movabs_hdu(fptr, hduIndex, &hduType, &status))
        {
            fits_close_file(fptr, &status);
            return false;
        }

        FitsHduSummary hdu;
        hdu.hduIndex = hduIndex;
        hdu.hduType = hduType;
        hdu.hduTypeName = hdu_type_name(hduType);
        hdu.extName = read_key_string(fptr, "EXTNAME");
        hdu.objectName = read_key_string(fptr, "OBJECT");
        hdu.filterName = read_key_string(fptr, "FILTER");
        hdu.dateObs = read_key_string(fptr, "DATE-OBS");

        if (!hdu.extName.empty())
            extNames.insert(hdu.extName);

        if (hduType == IMAGE_HDU)
        {
            int bitpix = 0;
            int naxis = 0;
            long naxes[3] = {1, 1, 1};
            int imageStatus = 0;
            if (!fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &imageStatus))
            {
                hdu.bitpix = bitpix;
                hdu.width = (naxis >= 1) ? static_cast<int>(naxes[0]) : 0;
                hdu.height = (naxis >= 2) ? static_cast<int>(naxes[1]) : 0;
                hdu.depth = (naxis >= 3) ? static_cast<int>(naxes[2]) : 1;
            }

            ++outSummary.imageHduCount;

            if (!firstImageCaptured)
            {
                firstImageCaptured = true;
                outSummary.width = hdu.width;
                outSummary.height = hdu.height;
                outSummary.depth = hdu.depth;
                outSummary.bitpix = hdu.bitpix;
                outSummary.objectName = hdu.objectName;
                outSummary.filterName = hdu.filterName;
                outSummary.dateObs = hdu.dateObs;
            }
        }

        if (outSummary.objectName.empty() && !hdu.objectName.empty())
            outSummary.objectName = hdu.objectName;
        if (outSummary.filterName.empty() && !hdu.filterName.empty())
            outSummary.filterName = hdu.filterName;
        if (outSummary.dateObs.empty() && !hdu.dateObs.empty())
            outSummary.dateObs = hdu.dateObs;

        outSummary.hdus.push_back(std::move(hdu));
    }

    std::ostringstream extOss;
    bool first = true;
    for (const std::string& extName : extNames)
    {
        if (!first)
            extOss << ", ";
        first = false;
        extOss << extName;
    }
    outSummary.extNames = extOss.str();
    outSummary.normalizedSearchText = normalize_search_text(
        outSummary.path + " " +
        outSummary.fileName + " " +
        outSummary.directory + " " +
        outSummary.objectName + " " +
        outSummary.filterName + " " +
        outSummary.dateObs + " " +
        outSummary.extNames);

    fits_close_file(fptr, &status);
    return true;
}

} // namespace kty
