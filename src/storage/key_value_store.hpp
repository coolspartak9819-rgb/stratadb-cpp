#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace stratadb {

class KeyValueStore {
public:
    explicit KeyValueStore(std::filesystem::path wal_path);
    ~KeyValueStore();

    KeyValueStore(const KeyValueStore&) = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    bool erase(const std::string& key);
    std::size_t size() const;
    void compact();

private:
    void replay_sstable();
    void replay();
    void append_record(char operation, const std::string& key, const std::string& value);
    void write_sstable();

    std::filesystem::path wal_path_;
    std::filesystem::path sstable_path_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> values_;
    std::ofstream wal_;
};

}  // namespace stratadb
