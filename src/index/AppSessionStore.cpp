#include "AppSessionStore.h"

#include <sqlite3.h>

#include <sstream>

namespace kty {

namespace {

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

static std::string column_text(sqlite3_stmt* stmt, int index)
{
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

static bool to_bool(const std::string& value, bool fallback)
{
    if (value == "1" || value == "true")
        return true;
    if (value == "0" || value == "false")
        return false;
    return fallback;
}

static int to_int(const std::string& value, int fallback)
{
    try
    {
        return std::stoi(value);
    }
    catch (...)
    {
        return fallback;
    }
}

static float to_float(const std::string& value, float fallback)
{
    try
    {
        return std::stof(value);
    }
    catch (...)
    {
        return fallback;
    }
}

static bool set_kv(sqlite3* db, const char* key, const std::string& value, std::string& errorMessage)
{
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO app_session_kv(key, value) VALUES (?1, ?2);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

} // namespace

bool AppSessionStore::initSchema(void* rawDb, std::string& errorMessage) const
{
    sqlite3* db = static_cast<sqlite3*>(rawDb);
    const char* sql = R"SQL(
CREATE TABLE IF NOT EXISTS app_session_kv (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS app_session_stack_files (
    ord INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    type INTEGER NOT NULL
);
)SQL";
    return exec_sql(db, sql, errorMessage);
}

bool AppSessionStore::load(const std::string& dbPath,
                           AppSessionData& outData,
                           std::string& errorMessage) const
{
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK)
    {
        errorMessage = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db)
            sqlite3_close(db);
        return false;
    }

    bool ok = initSchema(db, errorMessage);
    if (!ok)
    {
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* kvSql = "SELECT key, value FROM app_session_kv;";
    if (sqlite3_prepare_v2(db, kvSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const std::string key = column_text(stmt, 0);
        const std::string value = column_text(stmt, 1);

        if (key == "current_dir") outData.currentDir = value;
        else if (key == "current_path") outData.currentPath = value;
        else if (key == "bayer") outData.bayer = to_int(value, outData.bayer);
        else if (key == "use_bias") outData.useBias = to_bool(value, outData.useBias);
        else if (key == "use_dark") outData.useDark = to_bool(value, outData.useDark);
        else if (key == "use_flat") outData.useFlat = to_bool(value, outData.useFlat);
        else if (key == "reject_method") outData.rejectMethod = to_int(value, outData.rejectMethod);
        else if (key == "sigma_low") outData.sigmaLow = to_float(value, outData.sigmaLow);
        else if (key == "sigma_high") outData.sigmaHigh = to_float(value, outData.sigmaHigh);
        else if (key == "min_samples") outData.minSamples = to_int(value, outData.minSamples);
        else if (key == "denoise_enabled")
            outData.denoisePreviewMode = to_bool(value, true) ? 1 : 0;
        else if (key == "denoise_preview_mode")
            outData.denoisePreviewMode = to_int(value, outData.denoisePreviewMode);
        else if (key == "denoise_export_mode")
            outData.denoiseExportMode = to_int(value, outData.denoiseExportMode);
        else if (key == "denoise_strength") outData.denoiseStrength = to_float(value, outData.denoiseStrength);
        else if (key == "background_sigma") outData.backgroundSigma = to_float(value, outData.backgroundSigma);
        else if (key == "denoise_iterations") outData.denoiseIterations = to_int(value, outData.denoiseIterations);
    }
    sqlite3_finalize(stmt);

    const char* fileSql = "SELECT path, type FROM app_session_stack_files ORDER BY ord ASC;";
    if (sqlite3_prepare_v2(db, fileSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    outData.stackFiles.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        AppSessionStackFile item;
        item.path = column_text(stmt, 0);
        item.type = sqlite3_column_int(stmt, 1);
        outData.stackFiles.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool AppSessionStore::save(const std::string& dbPath,
                           const AppSessionData& data,
                           std::string& errorMessage) const
{
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK)
    {
        errorMessage = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db)
            sqlite3_close(db);
        return false;
    }

    bool ok = initSchema(db, errorMessage);
    if (!ok)
    {
        sqlite3_close(db);
        return false;
    }

    ok = exec_sql(db, "BEGIN IMMEDIATE TRANSACTION;", errorMessage);
    if (!ok)
    {
        sqlite3_close(db);
        return false;
    }

    ok = set_kv(db, "current_dir", data.currentDir, errorMessage) &&
         set_kv(db, "current_path", data.currentPath, errorMessage) &&
         set_kv(db, "bayer", std::to_string(data.bayer), errorMessage) &&
         set_kv(db, "use_bias", data.useBias ? "1" : "0", errorMessage) &&
         set_kv(db, "use_dark", data.useDark ? "1" : "0", errorMessage) &&
         set_kv(db, "use_flat", data.useFlat ? "1" : "0", errorMessage) &&
         set_kv(db, "reject_method", std::to_string(data.rejectMethod), errorMessage) &&
         set_kv(db, "sigma_low", std::to_string(data.sigmaLow), errorMessage) &&
         set_kv(db, "sigma_high", std::to_string(data.sigmaHigh), errorMessage) &&
         set_kv(db, "min_samples", std::to_string(data.minSamples), errorMessage) &&
         set_kv(db, "denoise_enabled", data.denoisePreviewMode != 0 ? "1" : "0", errorMessage) &&
         set_kv(db, "denoise_preview_mode", std::to_string(data.denoisePreviewMode), errorMessage) &&
         set_kv(db, "denoise_export_mode", std::to_string(data.denoiseExportMode), errorMessage) &&
         set_kv(db, "denoise_strength", std::to_string(data.denoiseStrength), errorMessage) &&
         set_kv(db, "background_sigma", std::to_string(data.backgroundSigma), errorMessage) &&
         set_kv(db, "denoise_iterations", std::to_string(data.denoiseIterations), errorMessage);

    if (ok)
        ok = exec_sql(db, "DELETE FROM app_session_stack_files;", errorMessage);

    if (ok)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO app_session_stack_files(ord, path, type) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            errorMessage = sqlite3_errmsg(db);
            ok = false;
        }
        else
        {
            for (size_t i = 0; i < data.stackFiles.size() && ok; ++i)
            {
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                sqlite3_bind_int(stmt, 1, static_cast<int>(i));
                sqlite3_bind_text(stmt, 2, data.stackFiles[i].path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 3, data.stackFiles[i].type);
                if (sqlite3_step(stmt) != SQLITE_DONE)
                {
                    errorMessage = sqlite3_errmsg(db);
                    ok = false;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    std::string txError;
    if (ok)
        exec_sql(db, "COMMIT;", txError);
    else
        exec_sql(db, "ROLLBACK;", txError);

    sqlite3_close(db);
    return ok;
}

} // namespace kty
