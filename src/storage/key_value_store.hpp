#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stratadb {

class KeyValueStore {
public:
    struct KeyInfo {
        std::string key;
        std::size_t value_size{0};
        std::int64_t expires_at_ms{0};
    };

    struct StorageStats {
        std::size_t keys{0};
        std::uint64_t last_sequence{0};
        std::uint64_t compactions_total{0};
        std::uintmax_t wal_bytes{0};
        std::uintmax_t sstable_bytes{0};
        std::uintmax_t replication_bytes{0};
        std::uintmax_t compaction_threshold_bytes{0};
    };

    explicit KeyValueStore(std::filesystem::path wal_path,
                           std::uintmax_t compaction_threshold_bytes = 4 * 1024 * 1024);
    ~KeyValueStore();

    KeyValueStore(const KeyValueStore&) = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    void put(const std::string& key, const std::string& value);
    void put(const std::string& key, const std::string& value, std::chrono::seconds ttl);
    std::optional<std::string> get(const std::string& key) const;
    bool erase(const std::string& key);
    std::size_t size() const;
    void compact();
    std::uint64_t compactions_total() const;
    std::string export_snapshot();
    void import_snapshot(const std::string& snapshot);
    std::string export_changes(std::uint64_t after_sequence) const;
    void import_changes(const std::string& changes);
    std::uint64_t last_sequence() const;
    std::vector<KeyInfo> list_keys() const;
    StorageStats storage_stats() const;

private:
    struct ValueEntry {
        std::string value;
        std::int64_t expires_at_ms{0};
    };

    struct JournalRecord {
        std::uint64_t sequence{0};
        char operation{0};
        std::string key;
        std::string payload;
    };

    static std::int64_t now_ms();
    static bool is_expired(const ValueEntry& entry, std::int64_t current_time_ms);
    std::optional<std::uint64_t> load_sstable(const std::filesystem::path& path,
                                              std::unordered_map<std::string, ValueEntry>& loaded_values) const;
    void replay_sstable();
    std::vector<JournalRecord> replay();
    std::unordered_set<std::uint64_t> replay_replication_index();
    void repair_replication_index(const std::vector<JournalRecord>& wal_records,
                                  std::unordered_set<std::uint64_t>& indexed_sequences);
    void append_record(std::uint64_t sequence, char operation, const std::string& key, const std::string& value);
    void append_replication_record(std::uint64_t sequence, char operation, const std::string& key, const std::string& payload);
    void write_sstable();
    void compact_locked();
    void compaction_loop();
    void request_compaction_if_needed();

    std::filesystem::path wal_path_;
    std::filesystem::path sstable_path_;
    std::filesystem::path replication_path_;
    std::uintmax_t compaction_threshold_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable compaction_condition_;
    std::unordered_map<std::string, ValueEntry> values_;
    std::ofstream wal_;
    std::uint64_t next_sequence_{1};
    std::thread compaction_thread_;
    bool stopping_{false};
    bool compaction_requested_{false};
    std::uint64_t compactions_total_{0};
};

}  // namespace stratadb
