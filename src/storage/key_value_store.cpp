#include "storage/key_value_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stratadb {
namespace {

constexpr char kPut = 'P';
constexpr char kDelete = 'D';
constexpr char kPutWithTtl = 'E';
constexpr char kSstableMagic[] = "STRATA02";
constexpr char kLegacySstableMagic[] = "STRATA01";
constexpr std::uint32_t kMaxRecordSize = 256 * 1024 * 1024;

void write_u32(std::ostream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool read_u32(std::istream& input, std::uint32_t& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

}  // namespace

KeyValueStore::KeyValueStore(std::filesystem::path wal_path, std::uintmax_t compaction_threshold_bytes)
    : wal_path_(std::move(wal_path)),
      sstable_path_(wal_path_.string() + ".sst"),
      compaction_threshold_bytes_(compaction_threshold_bytes) {
    if (compaction_threshold_bytes_ == 0) {
        throw std::invalid_argument("compaction threshold must be greater than zero");
    }
    if (wal_path_.has_parent_path()) {
        std::filesystem::create_directories(wal_path_.parent_path());
    }
    replay_sstable();
    replay();
    wal_.open(wal_path_, std::ios::binary | std::ios::app);
    if (!wal_) {
        throw std::runtime_error("unable to open WAL: " + wal_path_.string());
    }
    compaction_thread_ = std::thread(&KeyValueStore::compaction_loop, this);
}

KeyValueStore::~KeyValueStore() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    compaction_condition_.notify_one();
    if (compaction_thread_.joinable()) {
        compaction_thread_.join();
    }
    std::scoped_lock lock(mutex_);
    wal_.flush();
}

void KeyValueStore::put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        throw std::invalid_argument("key must not be empty");
    }
    std::scoped_lock lock(mutex_);
    append_record(kPut, key, value);
    values_[key] = ValueEntry{value, 0};
}

void KeyValueStore::put(const std::string& key, const std::string& value, std::chrono::seconds ttl) {
    if (key.empty()) {
        throw std::invalid_argument("key must not be empty");
    }
    if (ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("TTL must be greater than zero");
    }
    const auto expires_at_ms = now_ms() + ttl.count() * 1000;
    std::string payload(sizeof(expires_at_ms), '\0');
    std::memcpy(payload.data(), &expires_at_ms, sizeof(expires_at_ms));
    payload.append(value);
    std::scoped_lock lock(mutex_);
    append_record(kPutWithTtl, key, payload);
    values_[key] = ValueEntry{value, expires_at_ms};
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    std::scoped_lock lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end() || is_expired(found->second, now_ms())) {
        return std::nullopt;
    }
    return found->second.value;
}

bool KeyValueStore::erase(const std::string& key) {
    std::scoped_lock lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end() || is_expired(found->second, now_ms())) {
        return false;
    }
    append_record(kDelete, key, {});
    values_.erase(key);
    return true;
}

std::size_t KeyValueStore::size() const {
    std::scoped_lock lock(mutex_);
    const auto current_time_ms = now_ms();
    return static_cast<std::size_t>(std::count_if(values_.begin(), values_.end(), [current_time_ms](const auto& item) {
        return !is_expired(item.second, current_time_ms);
    }));
}

std::int64_t KeyValueStore::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool KeyValueStore::is_expired(const ValueEntry& entry, std::int64_t current_time_ms) {
    return entry.expires_at_ms != 0 && entry.expires_at_ms <= current_time_ms;
}

void KeyValueStore::compact() {
    std::scoped_lock lock(mutex_);
    compact_locked();
    compaction_requested_ = false;
}

std::uint64_t KeyValueStore::compactions_total() const {
    std::scoped_lock lock(mutex_);
    return compactions_total_;
}

std::string KeyValueStore::export_snapshot() {
    std::scoped_lock lock(mutex_);
    write_sstable();
    std::ifstream input(sstable_path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read snapshot: " + sstable_path_.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void KeyValueStore::import_snapshot(const std::string& snapshot) {
    if (snapshot.empty()) {
        throw std::invalid_argument("snapshot must not be empty");
    }
    std::scoped_lock lock(mutex_);
    const auto temporary_path = sstable_path_.string() + ".incoming";
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("unable to create incoming snapshot");
        }
        output.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("unable to write incoming snapshot");
        }
    }
    std::filesystem::rename(temporary_path, sstable_path_);
    wal_.close();
    std::ofstream truncate(wal_path_, std::ios::binary | std::ios::trunc);
    if (!truncate) {
        throw std::runtime_error("unable to truncate WAL during snapshot import");
    }
    truncate.close();
    values_.clear();
    replay_sstable();
    wal_.open(wal_path_, std::ios::binary | std::ios::app);
    if (!wal_) {
        throw std::runtime_error("unable to reopen WAL after snapshot import");
    }
    compaction_requested_ = false;
}

void KeyValueStore::compact_locked() {
    write_sstable();
    wal_.close();
    std::ofstream truncate(wal_path_, std::ios::binary | std::ios::trunc);
    if (!truncate) {
        throw std::runtime_error("unable to truncate WAL: " + wal_path_.string());
    }
    truncate.close();
    wal_.open(wal_path_, std::ios::binary | std::ios::app);
    if (!wal_) {
        throw std::runtime_error("unable to reopen WAL: " + wal_path_.string());
    }
    ++compactions_total_;
}

void KeyValueStore::compaction_loop() {
    while (true) {
        std::unique_lock lock(mutex_);
        compaction_condition_.wait(lock, [this] {
            return stopping_ || compaction_requested_;
        });
        if (stopping_) {
            return;
        }
        compaction_requested_ = false;
        try {
            compact_locked();
        } catch (const std::exception& error) {
            std::cerr << "StrataDB compaction failed: " << error.what() << '\n';
        }
    }
}

void KeyValueStore::request_compaction_if_needed() {
    std::error_code error;
    const auto wal_size = std::filesystem::file_size(wal_path_, error);
    if (!error && wal_size >= compaction_threshold_bytes_) {
        compaction_requested_ = true;
        compaction_condition_.notify_one();
    }
}

void KeyValueStore::replay_sstable() {
    std::ifstream input(sstable_path_, std::ios::binary);
    if (!input) {
        return;
    }

    char magic[sizeof(kSstableMagic) - 1]{};
    if (!input.read(magic, sizeof(magic))) {
        throw std::runtime_error("invalid SSTable header");
    }
    const bool current_format = std::equal(std::begin(magic), std::end(magic), std::begin(kSstableMagic));
    const bool legacy_format = std::equal(std::begin(magic), std::end(magic), std::begin(kLegacySstableMagic));
    if (!current_format && !legacy_format) {
        throw std::runtime_error("invalid SSTable header");
    }

    std::uint32_t record_count = 0;
    if (!read_u32(input, record_count) || record_count > 10000000) {
        throw std::runtime_error("invalid SSTable record count");
    }
    for (std::uint32_t index = 0; index < record_count; ++index) {
        std::uint32_t key_size = 0;
        std::uint32_t value_size = 0;
        if (!read_u32(input, key_size) || !read_u32(input, value_size) ||
            key_size == 0 || key_size > 16 * 1024 * 1024 || value_size > kMaxRecordSize) {
            throw std::runtime_error("invalid SSTable record");
        }
        std::int64_t expires_at_ms = 0;
        if (current_format && !input.read(reinterpret_cast<char*>(&expires_at_ms), sizeof(expires_at_ms))) {
            throw std::runtime_error("truncated SSTable expiration");
        }
        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(value.data(), static_cast<std::streamsize>(value_size))) {
            throw std::runtime_error("truncated SSTable record");
        }
        values_[std::move(key)] = ValueEntry{std::move(value), expires_at_ms};
    }
}

void KeyValueStore::replay() {
    std::ifstream input(wal_path_, std::ios::binary);
    if (!input) {
        return;
    }

    while (input.peek() != std::char_traits<char>::eof()) {
        char operation = 0;
        std::uint32_t key_size = 0;
        std::uint32_t value_size = 0;
        if (!input.get(operation) || !read_u32(input, key_size) || !read_u32(input, value_size)) {
            throw std::runtime_error("truncated WAL header");
        }
        if (key_size == 0 || key_size > 16 * 1024 * 1024 || value_size > kMaxRecordSize) {
            throw std::runtime_error("invalid WAL record size");
        }

        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(value.data(), static_cast<std::streamsize>(value_size))) {
            throw std::runtime_error("truncated WAL record");
        }
        if (operation == kPut) {
            values_[key] = ValueEntry{std::move(value), 0};
        } else if (operation == kPutWithTtl) {
            if (value.size() < sizeof(std::int64_t)) {
                throw std::runtime_error("invalid TTL WAL record");
            }
            std::int64_t expires_at_ms = 0;
            std::memcpy(&expires_at_ms, value.data(), sizeof(expires_at_ms));
            const auto actual_value = value.substr(sizeof(expires_at_ms));
            if (expires_at_ms > now_ms()) {
                values_[key] = ValueEntry{actual_value, expires_at_ms};
            }
        } else if (operation == kDelete) {
            values_.erase(key);
        } else {
            throw std::runtime_error("unknown WAL operation");
        }
    }
}

void KeyValueStore::write_sstable() {
    std::vector<std::pair<std::string, std::string>> records;
    records.reserve(values_.size());
    const auto current_time_ms = now_ms();
    for (const auto& [key, value] : values_) {
        if (!is_expired(value, current_time_ms)) {
            records.emplace_back(key, value.value);
        }
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    const auto temporary_path = sstable_path_.string() + ".tmp";
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create SSTable: " + temporary_path);
    }
    output.write(kSstableMagic, sizeof(kSstableMagic) - 1);
    write_u32(output, static_cast<std::uint32_t>(records.size()));
    for (const auto& [key, value] : records) {
        write_u32(output, static_cast<std::uint32_t>(key.size()));
        write_u32(output, static_cast<std::uint32_t>(value.size()));
        std::int64_t expires_at_ms = 0;
        const auto found = values_.find(key);
        if (found != values_.end()) {
            expires_at_ms = found->second.expires_at_ms;
        }
        output.write(reinterpret_cast<const char*>(&expires_at_ms), sizeof(expires_at_ms));
        output.write(key.data(), static_cast<std::streamsize>(key.size()));
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("unable to flush SSTable: " + sstable_path_.string());
    }
    output.close();
    std::filesystem::rename(temporary_path, sstable_path_);
}

void KeyValueStore::append_record(char operation, const std::string& key, const std::string& value) {
    const auto key_size = static_cast<std::uint32_t>(key.size());
    const auto value_size = static_cast<std::uint32_t>(value.size());
    wal_.put(operation);
    write_u32(wal_, key_size);
    write_u32(wal_, value_size);
    wal_.write(key.data(), static_cast<std::streamsize>(key.size()));
    wal_.write(value.data(), static_cast<std::streamsize>(value.size()));
    wal_.flush();
    if (!wal_) {
        throw std::runtime_error("unable to append WAL record");
    }
    request_compaction_if_needed();
}

}  // namespace stratadb
