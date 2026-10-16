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
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

static auto CharCast(const std::byte* data) { return reinterpret_cast<const char*>(data); }

bool DestroyDB(const std::string& path_str)
{
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

bool CDBWrapper::HasKeyStartingWith(const fs::path& path, uint8_t prefix)
{
    if (!fs::exists(path / "CURRENT")) return false;

    CBitcoinLevelDBLogger logger;
    leveldb::Options options;
    options.paranoid_checks = true;
    // Avoid creating or rotating LevelDB's LOG files during this probe.
    options.info_log = &logger;

    leveldb::DB* raw_db;
    HandleError(leveldb::DB::Open(options, fs::PathToString(path), &raw_db));
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

namespace {

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

} // namespace

CDBWrapper::CDBWrapper(const DBParams& params)
    : m_db_context{std::make_unique<LevelDBContext>(params)}, m_name{fs::PathToString(params.path.stem())}
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
