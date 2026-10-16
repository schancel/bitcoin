// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/filter_policy.h>
#include <leveldb/helpers/memenv/memenv.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>
#include <leveldb/write_batch.h>
#include <mdbx.h>
#include <random.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <util/byte_units.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/log.h>
#include <util/obfuscation.h>
#include <util/strencodings.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

static auto CharCast(const std::byte* data) { return reinterpret_cast<const char*>(data); }

namespace {
std::atomic<DBBackend> g_default_db_backend{DBBackend::LEVELDB};
} // namespace

void SetDefaultDBBackend(DBBackend backend) noexcept
{
    g_default_db_backend.store(backend, std::memory_order_relaxed);
}

DBBackend GetDefaultDBBackend() noexcept
{
    return g_default_db_backend.load(std::memory_order_relaxed);
}

std::optional<DBBackend> ParseDBBackend(std::string_view name) noexcept
{
    if (name == "leveldb") return DBBackend::LEVELDB;
    if (name == "mdbx") return DBBackend::MDBX;
    return std::nullopt;
}

bool DestroyDB(const std::string& path_str)
{
    const fs::path dir{fs::PathFromString(path_str)};
    std::error_code ec;
    fs::remove(dir / "mdbx.dat", ec);
    fs::remove(dir / "mdbx.lck", ec);
    fs::remove_all(dir / "leveldb.bak", ec);
    fs::remove_all(dir / "mdbx.bak", ec);
    return leveldb::DestroyDB(path_str, {}).ok();
}

/** Handle database error by throwing dbwrapper_error exception.
 */
static void HandleError(const leveldb::Status& status)
{
    if (status.ok())
        return;
    const std::string errmsg = "Fatal LevelDB error: " + status.ToString();
    LogError("%s", errmsg);
    LogInfo("You can use -debug=leveldb to get more complete diagnostic messages");
    throw dbwrapper_error(errmsg);
}

static void HandleMdbxError(int rc, const char* context)
{
    if (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
        return;
    }
    const std::string errmsg =
        std::string("Fatal libmdbx error (") + context + "): " +
        std::to_string(rc) + " (" + mdbx_strerror(rc) + ")";
    LogError("%s", errmsg);
    throw dbwrapper_error(errmsg);
}

class CBitcoinLevelDBLogger : public leveldb::Logger {
public:
    // This code is adapted from posix_logger.h, which is why it is using vsprintf.
    // Please do not do this in normal code
    void Logv(const char * format, va_list ap) override {
            if (!util::log::ShouldDebugLog(BCLog::LEVELDB)) {
                return;
            }
            char buffer[500];
            for (int iter = 0; iter < 2; iter++) {
                char* base;
                int bufsize;
                if (iter == 0) {
                    bufsize = sizeof(buffer);
                    base = buffer;
                }
                else {
                    bufsize = 30000;
                    base = new char[bufsize];
                }
                char* p = base;
                char* limit = base + bufsize;

                // Print the message
                if (p < limit) {
                    va_list backup_ap;
                    va_copy(backup_ap, ap);
                    // Do not use vsnprintf elsewhere in bitcoin source code, see above.
                    p += vsnprintf(p, limit - p, format, backup_ap);
                    va_end(backup_ap);
                }

                // Truncate to available space if necessary
                if (p >= limit) {
                    if (iter == 0) {
                        continue;       // Try again with larger buffer
                    }
                    else {
                        p = limit - 1;
                    }
                }

                // Add newline if necessary
                if (p == base || p[-1] != '\n') {
                    *p++ = '\n';
                }

                assert(p <= limit);
                base[std::min(bufsize - 1, (int)(p - base))] = '\0';
                LogDebug(BCLog::LEVELDB, "%s\n", util::RemoveSuffixView(base, "\n"));
                if (base != buffer) {
                    delete[] base;
                }
                break;
            }
    }
};

static void SetMaxOpenFiles(leveldb::Options *options) {
    // On most platforms the default setting of max_open_files (which is 1000)
    // is optimal. On Windows using a large file count is OK because the handles
    // do not interfere with select() loops. On 64-bit Unix hosts this value is
    // also OK, because up to that amount LevelDB will use an mmap
    // implementation that does not use extra file descriptors (the fds are
    // closed after being mmap'ed).
    //
    // Increasing the value beyond the default is dangerous because LevelDB will
    // fall back to a non-mmap implementation when the file count is too large.
    // On 32-bit Unix host we should decrease the value because the handles use
    // up real fds, and we want to avoid fd exhaustion issues.
    //
    // See PR #12495 for further discussion.

    int default_open_files = options->max_open_files;
#ifndef WIN32
    if (sizeof(void*) < 8) {
        options->max_open_files = 64;
    }
#endif
    LogDebug(BCLog::LEVELDB, "LevelDB using max_open_files=%d (default=%d)\n",
             options->max_open_files, default_open_files);
}

static leveldb::Options GetOptions(size_t nCacheSize, bool bloom_filter)
{
    leveldb::Options options;
    options.block_cache = leveldb::NewLRUCache(nCacheSize / 2);
    options.write_buffer_size = nCacheSize / 4; // up to two write buffers may be held in memory simultaneously
    options.filter_policy = bloom_filter ? leveldb::NewBloomFilterPolicy(10) : nullptr;
    options.compression = leveldb::kNoCompression;
    options.info_log = new CBitcoinLevelDBLogger();
    if (leveldb::kMajorVersion > 1 || (leveldb::kMajorVersion == 1 && leveldb::kMinorVersion >= 16)) {
        // LevelDB versions before 1.16 consider short writes to be corruption. Only trigger error
        // on corruption in later versions.
        options.paranoid_checks = true;
    }
    SetMaxOpenFiles(&options);
    return options;
}

static bool HasMdbxKeyStartingWith(const fs::path& db_dir, uint8_t prefix)
{
    MDBX_env* env{nullptr};
    HandleMdbxError(mdbx_env_create(&env), "mdbx_env_create(HasKeyStartingWith)");
    const int open_rc = mdbx_env_open(
        env, fs::PathToString(db_dir).c_str(),
        MDBX_env_flags_t(MDBX_RDONLY | MDBX_ACCEDE), 0600);
    if (open_rc != MDBX_SUCCESS) {
        mdbx_env_close(env);
        HandleMdbxError(open_rc, "mdbx_env_open(HasKeyStartingWith)");
    }
    MDBX_txn* txn{nullptr};
    int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS) {
        mdbx_env_close(env);
        HandleMdbxError(rc, "mdbx_txn_begin(HasKeyStartingWith)");
    }
    MDBX_dbi dbi{0};
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_ACCEDE, &dbi);
    if (rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        mdbx_env_close(env);
        HandleMdbxError(rc, "mdbx_dbi_open(HasKeyStartingWith)");
    }
    MDBX_cursor* cursor{nullptr};
    rc = mdbx_cursor_open(txn, dbi, &cursor);
    if (rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        mdbx_env_close(env);
        HandleMdbxError(rc, "mdbx_cursor_open(HasKeyStartingWith)");
    }
    MDBX_val k{&prefix, sizeof(prefix)};
    MDBX_val v{nullptr, 0};
    rc = mdbx_cursor_get(cursor, &k, &v, MDBX_SET_RANGE);
    bool found = false;
    if (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
        found = k.iov_len >= 1 && *static_cast<const uint8_t*>(k.iov_base) == prefix;
    } else if (rc != MDBX_NOTFOUND) {
        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        mdbx_env_close(env);
        HandleMdbxError(rc, "mdbx_cursor_get(HasKeyStartingWith)");
    }
    mdbx_cursor_close(cursor);
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return found;
}

static bool HasLevelDBKeyStartingWith(const fs::path& db_dir, uint8_t prefix)
{
    CBitcoinLevelDBLogger logger;
    leveldb::Options options;
    options.paranoid_checks = true;
    // Avoid creating or rotating LevelDB's LOG files during this probe.
    options.info_log = &logger;

    leveldb::DB* raw_db;
    HandleError(leveldb::DB::Open(options, fs::PathToString(db_dir), &raw_db));
    const std::unique_ptr<leveldb::DB> db{raw_db};

    leveldb::ReadOptions iteroptions;
    iteroptions.verify_checksums = true;
    iteroptions.fill_cache = false;
    const std::unique_ptr<leveldb::Iterator> it{db->NewIterator(iteroptions)};
    const leveldb::Slice prefix_slice{reinterpret_cast<const char*>(&prefix), sizeof(prefix)};
    it->Seek(prefix_slice);
    HandleError(it->status());
    return it->Valid() && it->key().starts_with(prefix_slice);
}

bool CDBWrapper::HasKeyStartingWith(const fs::path& path, uint8_t prefix)
{
    if (GetDefaultDBBackend() == DBBackend::MDBX) {
        if (fs::exists(path / "mdbx.dat")) return HasMdbxKeyStartingWith(path, prefix);
        if (fs::exists(path / "CURRENT")) return HasLevelDBKeyStartingWith(path, prefix);
        if (fs::exists(path / "leveldb.bak" / "CURRENT")) return HasLevelDBKeyStartingWith(path / "leveldb.bak", prefix);
        return false;
    }
    if (fs::exists(path / "CURRENT")) return HasLevelDBKeyStartingWith(path, prefix);
    if (fs::exists(path / "mdbx.dat")) return HasMdbxKeyStartingWith(path, prefix);
    if (fs::exists(path / "mdbx.bak" / "mdbx.dat")) return HasMdbxKeyStartingWith(path / "mdbx.bak", prefix);
    return false;
}

struct CDBBatch::WriteBatchImpl {
    virtual ~WriteBatchImpl() = default;
    virtual void Clear() = 0;
    virtual void Put(std::span<const std::byte> key, std::span<const std::byte> value) = 0;
    virtual void Delete(std::span<const std::byte> key) = 0;
    virtual size_t ApproximateSize() const = 0;
};

namespace {

struct LevelDBBatchImpl final : CDBBatch::WriteBatchImpl {
    leveldb::WriteBatch batch;

    void Clear() override { batch.Clear(); }
    void Put(std::span<const std::byte> key, std::span<const std::byte> value) override
    {
        batch.Put(leveldb::Slice(CharCast(key.data()), key.size()),
                  leveldb::Slice(CharCast(value.data()), value.size()));
    }
    void Delete(std::span<const std::byte> key) override
    {
        batch.Delete(leveldb::Slice(CharCast(key.data()), key.size()));
    }
    size_t ApproximateSize() const override { return batch.ApproximateSize(); }
};

struct MdbxBatchImpl final : CDBBatch::WriteBatchImpl {
    struct Op {
        bool is_delete;
        std::string key;
        std::string value;
    };
    std::vector<Op> ops;
    size_t approx_size{12};

    void Clear() override
    {
        ops.clear();
        approx_size = 12;
    }

    void Put(std::span<const std::byte> key, std::span<const std::byte> value) override
    {
        ops.push_back(Op{
            false,
            std::string(CharCast(key.data()), key.size()),
            std::string(CharCast(value.data()), value.size()),
        });
        approx_size += 1 + key.size() + value.size();
    }

    void Delete(std::span<const std::byte> key) override
    {
        ops.push_back(Op{
            true,
            std::string(CharCast(key.data()), key.size()),
            {},
        });
        approx_size += 1 + key.size();
    }

    size_t ApproximateSize() const override { return approx_size; }
};

} // namespace

CDBBatch::CDBBatch(const CDBWrapper& _parent)
    : parent{_parent},
      m_impl_batch{_parent.DBContext().NewBatch()}
{
    m_key_scratch.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
    m_value_scratch.reserve(DBWRAPPER_PREALLOC_VALUE_SIZE);
    Clear();
};

CDBBatch::~CDBBatch() = default;

void CDBBatch::Clear()
{
    m_impl_batch->Clear();
    assert(m_key_scratch.empty());
    assert(m_value_scratch.empty());
}

void CDBBatch::WriteImpl(std::span<const std::byte> key, DataStream& value)
{
    dbwrapper_private::GetObfuscation(parent)(value);
    m_impl_batch->Put(key, value);
}

void CDBBatch::EraseImpl(std::span<const std::byte> key)
{
    m_impl_batch->Delete(key);
}

size_t CDBBatch::ApproximateSize() const
{
    return m_impl_batch->ApproximateSize();
}

struct DBContext {
    virtual ~DBContext() = default;
    virtual std::optional<std::string> Read(std::span<const std::byte> key) const = 0;
    virtual bool Exists(std::span<const std::byte> key) const = 0;
    virtual void WriteBatch(CDBBatch::WriteBatchImpl& batch, bool fSync) = 0;
    virtual size_t EstimateSize(std::span<const std::byte> key1, std::span<const std::byte> key2) const = 0;
    virtual std::unique_ptr<CDBBatch::WriteBatchImpl> NewBatch() const = 0;
    virtual std::unique_ptr<CDBIterator::IteratorImpl> NewIterator() const = 0;
    virtual void CompactFull() = 0;
    virtual std::optional<std::string> GetProperty(const std::string& property) const = 0;
    virtual size_t DynamicMemoryUsage() const = 0;
};

namespace {

void MoveMdbxFilesToBackup(const fs::path& dir_path)
{
    std::error_code ec;
    const fs::path backup_dir = dir_path / "mdbx.bak";
    fs::remove_all(backup_dir, ec);
    fs::create_directories(backup_dir, ec);
    if (fs::exists(dir_path / "mdbx.dat")) {
        fs::rename(dir_path / "mdbx.dat", backup_dir / "mdbx.dat", ec);
    }
    if (fs::exists(dir_path / "mdbx.lck")) {
        fs::rename(dir_path / "mdbx.lck", backup_dir / "mdbx.lck", ec);
    }
}

void MoveLevelDBFilesToBackup(const fs::path& dir_path)
{
    std::error_code ec;
    const fs::path backup_dir = dir_path / "leveldb.bak";
    fs::remove_all(backup_dir, ec);
    fs::create_directories(backup_dir, ec);
    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        const fs::path filename{entry.path().filename()};
        const std::string name = fs::PathToString(filename);
        if (name == "mdbx.dat" || name == "mdbx.lck" || name == "leveldb.bak" || name == "mdbx.bak") {
            continue;
        }
        fs::rename(entry.path(), backup_dir / filename, ec);
    }
}

struct LevelDBContext final : DBContext {
    //! custom environment this database is using (may be nullptr in case of default environment)
    leveldb::Env* penv{nullptr};

    //! database options used
    leveldb::Options options;

    //! options used when reading from the database
    leveldb::ReadOptions readoptions;

    //! options used when iterating over values of the database
    leveldb::ReadOptions iteroptions;

    //! options used when writing to the database
    leveldb::WriteOptions writeoptions;

    //! options used when sync writing to the database
    leveldb::WriteOptions syncoptions;

    //! the database itself
    leveldb::DB* pdb{nullptr};

    explicit LevelDBContext(const DBParams& params);
    ~LevelDBContext() override;
    void MigrateFromMdbxIfPresent(const fs::path& dir_path);

    std::optional<std::string> Read(std::span<const std::byte> key) const override;
    bool Exists(std::span<const std::byte> key) const override;
    void WriteBatch(CDBBatch::WriteBatchImpl& batch, bool fSync) override;
    size_t EstimateSize(std::span<const std::byte> key1, std::span<const std::byte> key2) const override;
    std::unique_ptr<CDBBatch::WriteBatchImpl> NewBatch() const override;
    std::unique_ptr<CDBIterator::IteratorImpl> NewIterator() const override;
    void CompactFull() override;
    std::optional<std::string> GetProperty(const std::string& property) const override;
    size_t DynamicMemoryUsage() const override;
};

LevelDBContext::LevelDBContext(const DBParams& params)
{
    penv = nullptr;
    readoptions.verify_checksums = true;
    iteroptions.verify_checksums = true;
    iteroptions.fill_cache = false;
    syncoptions.sync = true;
    options = GetOptions(params.cache_bytes, params.bloom_filter);
    options.create_if_missing = true;
    options.max_file_size = params.max_file_size;
    assert(!(params.testing_env && params.memory_only));
    if (params.testing_env) {
        options.env = params.testing_env;
    } else if (params.memory_only) {
        penv = leveldb::NewMemEnv(leveldb::Env::Default());
        options.env = penv;
    }
    if (!params.memory_only) {
        if (params.wipe_data) {
            LogInfo("Wiping LevelDB in %s", fs::PathToString(params.path));
            std::error_code ec;
            fs::remove(params.path / "mdbx.dat", ec);
            fs::remove(params.path / "mdbx.lck", ec);
            fs::remove_all(params.path / "leveldb.bak", ec);
            fs::remove_all(params.path / "mdbx.bak", ec);
            leveldb::Status result = leveldb::DestroyDB(fs::PathToString(params.path), options);
            HandleError(result);
        }
        if (!params.testing_env) {
            TryCreateDirectories(params.path);
        }
        LogInfo("Opening LevelDB in %s", fs::PathToString(params.path));
    }
    // PathToString() return value is safe to pass to leveldb open function,
    // because on POSIX leveldb passes the byte string directly to ::open(), and
    // on Windows it converts from UTF-8 to UTF-16 before calling ::CreateFileW
    // (see env_posix.cc and env_windows.cc).
    leveldb::Status status = leveldb::DB::Open(options, fs::PathToString(params.path), &pdb);
    HandleError(status);
    if (!params.memory_only && !params.testing_env && !params.wipe_data) {
        MigrateFromMdbxIfPresent(params.path);
    }
    LogInfo("Opened LevelDB successfully");

    if (params.options.force_compact) {
        LogInfo("Starting database compaction of %s", fs::PathToString(params.path));
        CompactFull();
        LogInfo("Finished database compaction of %s", fs::PathToString(params.path));
    }
}

LevelDBContext::~LevelDBContext()
{
    delete pdb;
    pdb = nullptr;
    delete options.filter_policy;
    options.filter_policy = nullptr;
    delete options.info_log;
    options.info_log = nullptr;
    delete options.block_cache;
    options.block_cache = nullptr;
    delete penv;
    options.env = nullptr;
}

void LevelDBContext::WriteBatch(CDBBatch::WriteBatchImpl& batch, bool fSync)
{
    auto& ldb_batch = static_cast<LevelDBBatchImpl&>(batch);
    leveldb::Status status = pdb->Write(fSync ? syncoptions : writeoptions, &ldb_batch.batch);
    HandleError(status);
}

std::optional<std::string> LevelDBContext::GetProperty(const std::string& property) const
{
    if (std::string value; pdb->GetProperty(property, &value)) return value;
    return std::nullopt;
}

void LevelDBContext::CompactFull() { pdb->CompactRange(nullptr, nullptr); }

size_t LevelDBContext::DynamicMemoryUsage() const
{
    std::optional<size_t> parsed;
    if (auto memory{GetProperty("leveldb.approximate-memory-usage")}; !memory || !(parsed = ToIntegral<size_t>(*memory))) {
        LogDebug(BCLog::LEVELDB, "Failed to get approximate-memory-usage property\n");
        return 0;
    }
    return parsed.value();
}

std::optional<std::string> LevelDBContext::Read(std::span<const std::byte> key) const
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());
    std::string strValue;
    leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return std::nullopt;
        LogError("LevelDB read failure: %s", status.ToString());
        HandleError(status);
    }
    return strValue;
}

bool LevelDBContext::Exists(std::span<const std::byte> key) const
{
    leveldb::Slice slKey(CharCast(key.data()), key.size());

    std::string strValue;
    leveldb::Status status = pdb->Get(readoptions, slKey, &strValue);
    if (!status.ok()) {
        if (status.IsNotFound())
            return false;
        LogError("LevelDB read failure: %s", status.ToString());
        HandleError(status);
    }
    return true;
}

size_t LevelDBContext::EstimateSize(std::span<const std::byte> key1, std::span<const std::byte> key2) const
{
    leveldb::Slice slKey1(CharCast(key1.data()), key1.size());
    leveldb::Slice slKey2(CharCast(key2.data()), key2.size());
    uint64_t size = 0;
    leveldb::Range range(slKey1, slKey2);
    pdb->GetApproximateSizes(&range, 1, &size);
    return size;
}

std::unique_ptr<CDBBatch::WriteBatchImpl> LevelDBContext::NewBatch() const
{
    return std::make_unique<LevelDBBatchImpl>();
}

void LevelDBContext::MigrateFromMdbxIfPresent(const fs::path& dir_path)
{
    const bool has_active_mdbx = fs::exists(dir_path / "mdbx.dat");
    const bool has_backup_mdbx =
        !has_active_mdbx && !fs::exists(dir_path / "CURRENT") &&
        fs::exists(dir_path / "mdbx.bak" / "mdbx.dat");
    if (!has_active_mdbx && !has_backup_mdbx) {
        return;
    }
    const fs::path mdbx_dir = has_active_mdbx ? dir_path : (dir_path / "mdbx.bak");
    LogInfo("Migrating existing libmdbx database at %s to LevelDB...", fs::PathToString(mdbx_dir));

    MDBX_env* src_env{nullptr};
    HandleMdbxError(mdbx_env_create(&src_env), "mdbx_env_create(migrate)");
    const int open_rc = mdbx_env_open(
        src_env, fs::PathToString(mdbx_dir).c_str(),
        MDBX_env_flags_t(MDBX_RDONLY | MDBX_ACCEDE), 0600);
    if (open_rc != MDBX_SUCCESS) {
        mdbx_env_close(src_env);
        HandleMdbxError(open_rc, "mdbx_env_open(migrate)");
    }

    MDBX_txn* txn{nullptr};
    int rc = mdbx_txn_begin(src_env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS) {
        mdbx_env_close(src_env);
        HandleMdbxError(rc, "mdbx_txn_begin(migrate)");
    }
    MDBX_dbi dbi{0};
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_ACCEDE, &dbi);
    if (rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        mdbx_env_close(src_env);
        HandleMdbxError(rc, "mdbx_dbi_open(migrate)");
    }
    MDBX_cursor* cursor{nullptr};
    rc = mdbx_cursor_open(txn, dbi, &cursor);
    if (rc != MDBX_SUCCESS) {
        mdbx_txn_abort(txn);
        mdbx_env_close(src_env);
        HandleMdbxError(rc, "mdbx_cursor_open(migrate)");
    }

    delete pdb;
    pdb = nullptr;
    HandleError(leveldb::DestroyDB(fs::PathToString(dir_path), options));
    HandleError(leveldb::DB::Open(options, fs::PathToString(dir_path), &pdb));

    leveldb::WriteBatch batch;
    size_t migrated_entries = 0;
    size_t batch_bytes = 0;
    MDBX_val k{nullptr, 0};
    MDBX_val v{nullptr, 0};
    rc = mdbx_cursor_get(cursor, &k, &v, MDBX_FIRST);
    while (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
        batch.Put(
            leveldb::Slice(static_cast<const char*>(k.iov_base), k.iov_len),
            leveldb::Slice(static_cast<const char*>(v.iov_base), v.iov_len));
        ++migrated_entries;
        batch_bytes += k.iov_len + v.iov_len;
        if (batch_bytes >= 16 * 1024 * 1024) {
            HandleError(pdb->Write(writeoptions, &batch));
            batch.Clear();
            batch_bytes = 0;
        }
        rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT);
    }
    mdbx_cursor_close(cursor);
    mdbx_txn_abort(txn);
    mdbx_env_close(src_env);
    if (rc != MDBX_NOTFOUND) {
        HandleMdbxError(rc, "mdbx_cursor_get(migrate)");
    }
    if (batch_bytes > 0 || migrated_entries > 0) {
        HandleError(pdb->Write(syncoptions, &batch));
    }

    if (has_active_mdbx) {
        MoveMdbxFilesToBackup(dir_path);
    }
    LogInfo("Migrated %zu entries from libmdbx to LevelDB at %s (backup kept in %s)",
            migrated_entries, fs::PathToString(dir_path),
            fs::PathToString(dir_path / "mdbx.bak"));
}

struct MdbxContext final : DBContext {
    MDBX_env* env{nullptr};
    MDBX_dbi dbi{0};
    fs::path db_dir;
    bool is_memory{false};
    mutable std::mutex write_mutex;

    void OpenMdbxEnvironment(size_t nCacheSize)
    {
        int rc = mdbx_env_create(&env);
        HandleMdbxError(rc, "mdbx_env_create");

        rc = mdbx_env_set_maxreaders(env, 256);
        HandleMdbxError(rc, "mdbx_env_set_maxreaders");

        const intptr_t size_upper = (sizeof(void*) >= 8)
            ? static_cast<intptr_t>(256ULL * 1024ULL * 1024ULL * 1024ULL)
            : static_cast<intptr_t>(512ULL * 1024ULL * 1024ULL);
        const intptr_t growth_step = static_cast<intptr_t>(
            std::clamp<size_t>(nCacheSize, 4 * 1024 * 1024, 64 * 1024 * 1024));
        const intptr_t shrink_threshold = growth_step * 2;
        rc = mdbx_env_set_geometry(
            env,
            /*size_lower=*/4096 * 16,
            /*size_now=*/4096 * 64,
            /*size_upper=*/size_upper,
            /*growth_step=*/growth_step,
            /*shrink_threshold=*/shrink_threshold,
            /*pagesize=*/4096);
        HandleMdbxError(rc, "mdbx_env_set_geometry");

        const MDBX_env_flags_t flags = MDBX_env_flags_t(
            MDBX_WRITEMAP | MDBX_SAFE_NOSYNC | MDBX_NOMETASYNC | MDBX_LIFORECLAIM | MDBX_NORDAHEAD);

        rc = mdbx_env_open(env, fs::PathToString(db_dir).c_str(), flags, 0600);
        HandleMdbxError(rc, "mdbx_env_open");

        rc = mdbx_env_set_syncbytes(env, 64 * 1024 * 1024);
        HandleMdbxError(rc, "mdbx_env_set_syncbytes");
        rc = mdbx_env_set_syncperiod(env, 655360);
        HandleMdbxError(rc, "mdbx_env_set_syncperiod");

        MDBX_txn* txn{nullptr};
        rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
        HandleMdbxError(rc, "mdbx_txn_begin(open)");
        rc = mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &dbi);
        if (rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            HandleMdbxError(rc, "mdbx_dbi_open");
        }
        rc = mdbx_txn_commit(txn);
        HandleMdbxError(rc, "mdbx_txn_commit(open)");
    }

    void MigrateFromLevelDBIfPresent(size_t nCacheSize)
    {
        std::error_code ec;
        const bool has_active_leveldb = fs::exists(db_dir / "CURRENT");
        const bool has_backup_leveldb =
            !has_active_leveldb && !fs::exists(db_dir / "mdbx.dat") &&
            fs::exists(db_dir / "leveldb.bak" / "CURRENT");
        if (!has_active_leveldb && !has_backup_leveldb) {
            return;
        }
        const fs::path ldb_dir = has_active_leveldb ? db_dir : (db_dir / "leveldb.bak");
        LogInfo("Migrating existing LevelDB database at %s to libmdbx...", fs::PathToString(ldb_dir));

        if (env) {
            mdbx_env_close(env);
            env = nullptr;
        }
        fs::remove(db_dir / "mdbx.dat", ec);
        fs::remove(db_dir / "mdbx.lck", ec);
        OpenMdbxEnvironment(nCacheSize);

        leveldb::Options ldb_options = GetOptions(nCacheSize, /*bloom_filter=*/false);
        ldb_options.create_if_missing = false;
        leveldb::DB* ldb{nullptr};
        leveldb::Status status = leveldb::DB::Open(ldb_options, fs::PathToString(ldb_dir), &ldb);
        if (!status.ok()) {
            delete ldb_options.filter_policy;
            delete ldb_options.info_log;
            delete ldb_options.block_cache;
            HandleError(status);
        }

        leveldb::ReadOptions iter_opts;
        iter_opts.verify_checksums = true;
        iter_opts.fill_cache = false;
        std::unique_ptr<leveldb::Iterator> it(ldb->NewIterator(iter_opts));

        MDBX_txn* txn{nullptr};
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
        HandleMdbxError(rc, "mdbx_txn_begin(migrate)");

        size_t migrated_entries = 0;
        size_t batch_bytes = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            const leveldb::Slice k = it->key();
            const leveldb::Slice v = it->value();
            MDBX_val mk{const_cast<char*>(k.data()), k.size()};
            MDBX_val mv{const_cast<char*>(v.data()), v.size()};
            rc = mdbx_put(txn, dbi, &mk, &mv, MDBX_UPSERT);
            if (rc != MDBX_SUCCESS) {
                mdbx_txn_abort(txn);
                it.reset();
                delete ldb;
                delete ldb_options.filter_policy;
                delete ldb_options.info_log;
                delete ldb_options.block_cache;
                HandleMdbxError(rc, "mdbx_put(migrate)");
            }
            ++migrated_entries;
            batch_bytes += k.size() + v.size();
            if (batch_bytes >= 64 * 1024 * 1024) {
                rc = mdbx_txn_commit(txn);
                HandleMdbxError(rc, "mdbx_txn_commit(migrate-chunk)");
                rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
                HandleMdbxError(rc, "mdbx_txn_begin(migrate-chunk)");
                batch_bytes = 0;
            }
        }

        const leveldb::Status iter_status = it->status();
        it.reset();
        delete ldb;
        delete ldb_options.filter_policy;
        delete ldb_options.info_log;
        delete ldb_options.block_cache;
        if (!iter_status.ok()) {
            mdbx_txn_abort(txn);
            HandleError(iter_status);
        }

        rc = mdbx_txn_commit(txn);
        HandleMdbxError(rc, "mdbx_txn_commit(migrate)");
        rc = mdbx_env_sync_ex(env, /*force=*/true, /*nonblock=*/false);
        if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
            HandleMdbxError(rc, "mdbx_env_sync_ex(migrate)");
        }

        if (has_active_leveldb) {
            MoveLevelDBFilesToBackup(db_dir);
        }
        LogInfo("Migrated %zu entries from LevelDB to libmdbx at %s (backup kept in %s)",
                migrated_entries, fs::PathToString(db_dir),
                fs::PathToString(db_dir / "leveldb.bak"));
    }

    explicit MdbxContext(const DBParams& params)
        : is_memory{params.memory_only}
    {
        if (is_memory) {
            static std::atomic<uint64_t> mem_counter{0};
            const uint64_t id = ++mem_counter;
            db_dir = fs::path{fs::temp_directory_path()} /
                     fs::PathFromString("bitcoin-mdbx-mem-" + std::to_string(id) + "-" +
                                        std::to_string(FastRandomContext().rand64()));
            std::error_code ec;
            fs::remove_all(db_dir, ec);
            TryCreateDirectories(db_dir);
        } else {
            db_dir = params.path;
            if (params.wipe_data) {
                LogInfo("Wiping libmdbx in %s", fs::PathToString(db_dir));
                DestroyDB(fs::PathToString(db_dir));
            }
            TryCreateDirectories(db_dir);
            LogInfo("Opening libmdbx in %s", fs::PathToString(db_dir));
        }

        OpenMdbxEnvironment(params.cache_bytes);

        if (!is_memory && !params.wipe_data) {
            MigrateFromLevelDBIfPresent(params.cache_bytes);
        }

        if (!is_memory) {
            LogInfo("Opened libmdbx successfully");
        }
    }

    ~MdbxContext() override
    {
        if (env) {
            mdbx_env_close(env);
            env = nullptr;
        }
        if (is_memory && !db_dir.empty()) {
            std::error_code ec;
            fs::remove_all(db_dir, ec);
        }
    }

    std::optional<std::string> Read(std::span<const std::byte> key) const override
    {
        MDBX_txn* txn{nullptr};
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        HandleMdbxError(rc, "mdbx_txn_begin(Read)");

        static const std::byte empty_byte{0};
        MDBX_val k{const_cast<std::byte*>(key.empty() ? &empty_byte : key.data()), key.size()};
        MDBX_val v{nullptr, 0};
        rc = mdbx_get(txn, dbi, &k, &v);
        if (rc == MDBX_NOTFOUND) {
            mdbx_txn_abort(txn);
            return std::nullopt;
        }
        if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
            mdbx_txn_abort(txn);
            HandleMdbxError(rc, "mdbx_get(Read)");
        }
        std::string result(static_cast<const char*>(v.iov_base), v.iov_len);
        mdbx_txn_abort(txn);
        return result;
    }

    bool Exists(std::span<const std::byte> key) const override
    {
        MDBX_txn* txn{nullptr};
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        HandleMdbxError(rc, "mdbx_txn_begin(Exists)");

        static const std::byte empty_byte{0};
        MDBX_val k{const_cast<std::byte*>(key.empty() ? &empty_byte : key.data()), key.size()};
        MDBX_val v{nullptr, 0};
        rc = mdbx_get(txn, dbi, &k, &v);
        mdbx_txn_abort(txn);
        if (rc == MDBX_NOTFOUND) {
            return false;
        }
        HandleMdbxError(rc, "mdbx_get(Exists)");
        return true;
    }

    void WriteBatch(CDBBatch::WriteBatchImpl& batch, bool fSync) override
    {
        auto& mdbx_batch = static_cast<MdbxBatchImpl&>(batch);
        std::lock_guard<std::mutex> lock(write_mutex);

        if (!mdbx_batch.ops.empty()) {
            MDBX_txn* txn{nullptr};
            int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
            HandleMdbxError(rc, "mdbx_txn_begin(WriteBatch)");

            static const char empty_byte = 0;
            for (const auto& op : mdbx_batch.ops) {
                MDBX_val k{
                    const_cast<char*>(op.key.empty() ? &empty_byte : op.key.data()),
                    op.key.size()};
                if (op.is_delete) {
                    rc = mdbx_del(txn, dbi, &k, nullptr);
                    if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                        mdbx_txn_abort(txn);
                        HandleMdbxError(rc, "mdbx_del(WriteBatch)");
                    }
                } else {
                    MDBX_val v{
                        const_cast<char*>(op.value.empty() ? &empty_byte : op.value.data()),
                        op.value.size()};
                    rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
                    if (rc != MDBX_SUCCESS) {
                        mdbx_txn_abort(txn);
                        HandleMdbxError(rc, "mdbx_put(WriteBatch)");
                    }
                }
            }

            rc = mdbx_txn_commit(txn);
            HandleMdbxError(rc, "mdbx_txn_commit(WriteBatch)");
        }

        if (fSync && !is_memory) {
            const int rc = mdbx_env_sync_ex(env, /*force=*/true, /*nonblock=*/false);
            if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
                HandleMdbxError(rc, "mdbx_env_sync_ex(WriteBatch)");
            }
        }
    }

    size_t EstimateSize(std::span<const std::byte> key1, std::span<const std::byte> key2) const override
    {
        MDBX_txn* txn{nullptr};
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        if (rc != MDBX_SUCCESS) return 0;

        MDBX_cursor* cursor{nullptr};
        rc = mdbx_cursor_open(txn, dbi, &cursor);
        if (rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return 0;
        }

        static const std::byte empty_byte{0};
        MDBX_val k{const_cast<std::byte*>(key1.empty() ? &empty_byte : key1.data()), key1.size()};
        MDBX_val v{nullptr, 0};
        size_t total = 0;
        rc = key1.empty()
            ? mdbx_cursor_get(cursor, &k, &v, MDBX_FIRST)
            : mdbx_cursor_get(cursor, &k, &v, MDBX_SET_RANGE);
        while (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
            const int cmp = std::memcmp(
                k.iov_base, key2.data(), std::min(k.iov_len, key2.size()));
            if (cmp > 0 || (cmp == 0 && k.iov_len >= key2.size())) {
                break;
            }
            total += k.iov_len + v.iov_len;
            rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT);
        }

        mdbx_cursor_close(cursor);
        mdbx_txn_abort(txn);
        return total;
    }

    std::unique_ptr<CDBBatch::WriteBatchImpl> NewBatch() const override
    {
        return std::make_unique<MdbxBatchImpl>();
    }

    std::unique_ptr<CDBIterator::IteratorImpl> NewIterator() const override;

    void CompactFull() override
    {
        if (!env || is_memory) return;
        const int rc = mdbx_env_sync_ex(env, /*force=*/true, /*nonblock=*/false);
        if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
            HandleMdbxError(rc, "mdbx_env_sync_ex(CompactFull)");
        }
    }

    std::optional<std::string> GetProperty(const std::string& property) const override
    {
        if (property == "leveldb.approximate-memory-usage") {
            return std::to_string(DynamicMemoryUsage());
        }
        return std::nullopt;
    }

    size_t DynamicMemoryUsage() const override
    {
        if (!env) return 0;
        MDBX_txn* txn{nullptr};
        if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS) {
            return 0;
        }
        MDBX_stat stat{};
        size_t usage = 0;
        if (mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)) == MDBX_SUCCESS) {
            usage = static_cast<size_t>(stat.ms_psize) *
                    static_cast<size_t>(stat.ms_branch_pages + stat.ms_leaf_pages + stat.ms_overflow_pages);
        }
        mdbx_txn_abort(txn);
        return usage;
    }
};

std::unique_ptr<DBContext> MakeDBContext(const DBParams& params)
{
    const DBBackend backend = params.backend.value_or(
        params.testing_env ? DBBackend::LEVELDB : GetDefaultDBBackend());
    switch (backend) {
    case DBBackend::MDBX:
        return std::make_unique<MdbxContext>(params);
    case DBBackend::LEVELDB:
        return std::make_unique<LevelDBContext>(params);
    }
    assert(false);
}

} // namespace

CDBWrapper::CDBWrapper(const DBParams& params)
    : m_db_context{MakeDBContext(params)}, m_name{fs::PathToString(params.path.stem())}
{
    if (!Read(OBFUSCATION_KEY, m_obfuscation) && params.obfuscate && IsEmpty()) {
        // Generate and write the new obfuscation key.
        const Obfuscation obfuscation{FastRandomContext{}.randbytes<Obfuscation::KEY_SIZE>()};
        assert(!m_obfuscation); // Make sure the key is written without obfuscation.
        Write(OBFUSCATION_KEY, obfuscation);
        m_obfuscation = obfuscation;
        LogInfo("Wrote new obfuscation key for %s: %s", fs::PathToString(params.path), m_obfuscation.HexKey());
    }
    LogInfo("Using obfuscation key for %s: %s", fs::PathToString(params.path), m_obfuscation.HexKey());
}

CDBWrapper::~CDBWrapper() = default;

void CDBWrapper::WriteBatch(CDBBatch& batch, bool fSync)
{
    const bool log_memory = util::log::ShouldDebugLog(BCLog::LEVELDB);
    double mem_before = 0;
    if (log_memory) {
        mem_before = DynamicMemoryUsage() / double(1_MiB);
    }
    DBContext().WriteBatch(*batch.m_impl_batch, fSync);
    if (log_memory) {
        double mem_after{DynamicMemoryUsage() / double(1_MiB)};
        LogDebug(BCLog::LEVELDB, "WriteBatch memory usage: db=%s, before=%.1fMiB, after=%.1fMiB\n",
                 m_name, mem_before, mem_after);
    }
}

std::optional<std::string> CDBWrapper::GetProperty(const std::string& property) const
{
    return DBContext().GetProperty(property);
}

void CDBWrapper::CompactFull() { DBContext().CompactFull(); }

size_t CDBWrapper::DynamicMemoryUsage() const
{
    return DBContext().DynamicMemoryUsage();
}

std::optional<std::string> CDBWrapper::ReadImpl(std::span<const std::byte> key) const
{
    return DBContext().Read(key);
}

bool CDBWrapper::ExistsImpl(std::span<const std::byte> key) const
{
    return DBContext().Exists(key);
}

size_t CDBWrapper::EstimateSizeImpl(std::span<const std::byte> key1, std::span<const std::byte> key2) const
{
    return DBContext().EstimateSize(key1, key2);
}

bool CDBWrapper::IsEmpty()
{
    std::unique_ptr<CDBIterator> it(NewIterator());
    it->SeekToFirst();
    return !(it->Valid());
}

struct CDBIterator::IteratorImpl {
    virtual ~IteratorImpl() = default;
    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Seek(std::span<const std::byte> key) = 0;
    virtual void Next() = 0;
    virtual std::span<const std::byte> GetKey() const = 0;
    virtual std::span<const std::byte> GetValue() const = 0;
};

namespace {

struct LevelDBIteratorImpl final : CDBIterator::IteratorImpl {
    const std::unique_ptr<leveldb::Iterator> iter;

    explicit LevelDBIteratorImpl(leveldb::Iterator* _iter) : iter{_iter} {}

    bool Valid() const override { return iter->Valid(); }
    void SeekToFirst() override { iter->SeekToFirst(); }
    void Seek(std::span<const std::byte> key) override
    {
        leveldb::Slice slKey(CharCast(key.data()), key.size());
        iter->Seek(slKey);
    }
    void Next() override { iter->Next(); }
    std::span<const std::byte> GetKey() const override { return MakeByteSpan(iter->key()); }
    std::span<const std::byte> GetValue() const override { return MakeByteSpan(iter->value()); }
};

std::unique_ptr<CDBIterator::IteratorImpl> LevelDBContext::NewIterator() const
{
    return std::make_unique<LevelDBIteratorImpl>(pdb->NewIterator(iteroptions));
}

struct MdbxIteratorImpl final : CDBIterator::IteratorImpl {
    MDBX_txn* txn{nullptr};
    MDBX_cursor* cursor{nullptr};
    bool valid{false};
    MDBX_val current_key{nullptr, 0};
    MDBX_val current_val{nullptr, 0};

    MdbxIteratorImpl(MDBX_env* env, MDBX_dbi dbi)
    {
        if (!env) return;
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
        HandleMdbxError(rc, "mdbx_txn_begin(iterator)");
        rc = mdbx_cursor_open(txn, dbi, &cursor);
        if (rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            txn = nullptr;
            HandleMdbxError(rc, "mdbx_cursor_open(iterator)");
        }
    }

    ~MdbxIteratorImpl() override
    {
        if (cursor) {
            mdbx_cursor_close(cursor);
            cursor = nullptr;
        }
        if (txn) {
            mdbx_txn_abort(txn);
            txn = nullptr;
        }
    }

    void Step(MDBX_cursor_op op)
    {
        if (!cursor) {
            valid = false;
            return;
        }
        const int rc = mdbx_cursor_get(cursor, &current_key, &current_val, op);
        if (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
            valid = true;
        } else if (rc == MDBX_NOTFOUND || rc == MDBX_ENODATA) {
            valid = false;
        } else {
            valid = false;
            HandleMdbxError(rc, "mdbx_cursor_get");
        }
    }

    bool Valid() const override { return valid; }

    void SeekToFirst() override { Step(MDBX_FIRST); }

    void Seek(std::span<const std::byte> key) override
    {
        if (!cursor) {
            valid = false;
            return;
        }
        if (key.empty()) {
            Step(MDBX_FIRST);
            return;
        }
        current_key.iov_base = const_cast<std::byte*>(key.data());
        current_key.iov_len = key.size();
        Step(MDBX_SET_RANGE);
    }

    void Next() override { Step(MDBX_NEXT); }

    std::span<const std::byte> GetKey() const override
    {
        assert(valid);
        return {static_cast<const std::byte*>(current_key.iov_base), current_key.iov_len};
    }

    std::span<const std::byte> GetValue() const override
    {
        assert(valid);
        return {static_cast<const std::byte*>(current_val.iov_base), current_val.iov_len};
    }
};

std::unique_ptr<CDBIterator::IteratorImpl> MdbxContext::NewIterator() const
{
    return std::make_unique<MdbxIteratorImpl>(env, dbi);
}

} // namespace

CDBIterator::CDBIterator(const CDBWrapper& _parent, std::unique_ptr<IteratorImpl> _piter) : parent(_parent),
                                                                                            m_impl_iter(std::move(_piter))
{
    m_scratch.reserve(DBWRAPPER_PREALLOC_KEY_SIZE);
}

CDBIterator* CDBWrapper::NewIterator()
{
    return new CDBIterator{*this, DBContext().NewIterator()};
}

void CDBIterator::SeekImpl(std::span<const std::byte> key)
{
    m_impl_iter->Seek(key);
}

std::span<const std::byte> CDBIterator::GetKeyImpl() const
{
    // The returned span borrows from the current iterator entry and is only
    // valid until the iterator is advanced.
    return m_impl_iter->GetKey();
}

std::span<const std::byte> CDBIterator::GetValueImpl() const
{
    return m_impl_iter->GetValue();
}

CDBIterator::~CDBIterator() = default;
bool CDBIterator::Valid() const { return m_impl_iter->Valid(); }
void CDBIterator::SeekToFirst() { m_impl_iter->SeekToFirst(); }
void CDBIterator::Next() { m_impl_iter->Next(); }

namespace dbwrapper_private {

const Obfuscation& GetObfuscation(const CDBWrapper& w)
{
    return w.m_obfuscation;
}

} // namespace dbwrapper_private
