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

namespace stratadb {

class KeyValueStore {
public:
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

private:
    struct ValueEntry {
        std::string value;
        std::int64_t expires_at_ms{0};
    };

    static std::int64_t now_ms();
    static bool is_expired(const ValueEntry& entry, std::int64_t current_time_ms);
    void replay_sstable();
    void replay();
    void replay_replication_index();
    void append_record(char operation, const std::string& key, const std::string& value);
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
