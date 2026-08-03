#include "storage/key_value_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace stratadb {
namespace {

constexpr char kPut = 'P';
constexpr char kDelete = 'D';
constexpr char kPutWithTtl = 'E';
constexpr char kSequencedPut = 'p';
constexpr char kSequencedDelete = 'd';
constexpr char kSequencedPutWithTtl = 'e';
constexpr char kSstableMagic[] = "STRATA03";
constexpr char kTtlSstableMagic[] = "STRATA02";
constexpr char kLegacySstableMagic[] = "STRATA01";
constexpr char kChangesMagic[] = "STRCHG01";
constexpr std::uint32_t kMaxRecordSize = 256 * 1024 * 1024;

void write_u32(std::ostream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u64(std::ostream& output, std::uint64_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool read_u32(std::istream& input, std::uint32_t& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool read_u64(std::istream& input, std::uint64_t& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

std::uint32_t crc32_update(std::uint32_t crc, const char* data, std::size_t size) {
    crc = ~crc;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<unsigned char>(data[index]);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

template <typename T>
std::uint32_t crc32_update_value(std::uint32_t crc, const T& value) {
    return crc32_update(crc, reinterpret_cast<const char*>(&value), sizeof(value));
}

std::uint32_t wal_record_checksum(std::uint64_t sequence, char operation,
                                  const std::string& key, const std::string& value) {
    auto checksum = crc32_update_value(0, sequence);
    checksum = crc32_update(checksum, &operation, sizeof(operation));
    const auto key_size = static_cast<std::uint32_t>(key.size());
    const auto value_size = static_cast<std::uint32_t>(value.size());
    checksum = crc32_update_value(checksum, key_size);
    checksum = crc32_update_value(checksum, value_size);
    checksum = crc32_update(checksum, key.data(), key.size());
    checksum = crc32_update(checksum, value.data(), value.size());
    return checksum;
}

void fsync_file(const std::filesystem::path& path, const std::string& context) {
    const auto native_path = path.string();
    const int fd = ::open(native_path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error(context + ": " + std::strerror(errno));
    }
    if (::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(context + ": " + std::strerror(error));
    }
    if (::close(fd) != 0) {
        throw std::runtime_error(context + ": " + std::strerror(errno));
    }
}

void fsync_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    const auto native_path = parent.string();
#ifdef O_DIRECTORY
    const int fd = ::open(native_path.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int fd = ::open(native_path.c_str(), O_RDONLY);
#endif
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

void truncate_file_to(const std::filesystem::path& path, std::uintmax_t size, const std::string& context) {
    std::filesystem::resize_file(path, size);
    fsync_file(path, context);
}

char to_wal_operation(char operation) {
    if (operation == kPut) {
        return kSequencedPut;
    }
    if (operation == kDelete) {
        return kSequencedDelete;
    }
    if (operation == kPutWithTtl) {
        return kSequencedPutWithTtl;
    }
    throw std::runtime_error("unknown WAL operation");
}

std::optional<char> from_sequenced_wal_operation(char operation) {
    if (operation == kSequencedPut) {
        return kPut;
    }
    if (operation == kSequencedDelete) {
        return kDelete;
    }
    if (operation == kSequencedPutWithTtl) {
        return kPutWithTtl;
    }
    return std::nullopt;
}

}  // namespace

KeyValueStore::KeyValueStore(std::filesystem::path wal_path, std::uintmax_t compaction_threshold_bytes)
    : wal_path_(std::move(wal_path)),
      sstable_path_(wal_path_.string() + ".sst"),
      replication_path_(wal_path_.string() + ".repl"),
      compaction_threshold_bytes_(compaction_threshold_bytes) {
    if (compaction_threshold_bytes_ == 0) {
        throw std::invalid_argument("compaction threshold must be greater than zero");
    }
    if (wal_path_.has_parent_path()) {
        std::filesystem::create_directories(wal_path_.parent_path());
    }
    replay_sstable();
    const auto sequenced_wal_records = replay();
    auto indexed_sequences = replay_replication_index();
    repair_replication_index(sequenced_wal_records, indexed_sequences);
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
    const auto sequence = next_sequence_++;
    append_record(sequence, kPut, key, value);
    append_replication_record(sequence, kPut, key, value);
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
    const auto sequence = next_sequence_++;
    append_record(sequence, kPutWithTtl, key, payload);
    append_replication_record(sequence, kPutWithTtl, key, payload);
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
    const auto sequence = next_sequence_++;
    append_record(sequence, kDelete, key, {});
    append_replication_record(sequence, kDelete, key, {});
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
    fsync_file(temporary_path, "unable to fsync incoming snapshot");
    std::unordered_map<std::string, ValueEntry> snapshot_values;
    std::optional<std::uint64_t> snapshot_sequence;
    try {
        snapshot_sequence = load_sstable(temporary_path, snapshot_values);
        if (!snapshot_sequence) {
            throw std::invalid_argument("snapshot must contain an SSTable");
        }
    } catch (...) {
        std::filesystem::remove(temporary_path);
        throw;
    }
    std::filesystem::rename(temporary_path, sstable_path_);
    fsync_parent_directory(sstable_path_);
    wal_.close();
    std::ofstream truncate(wal_path_, std::ios::binary | std::ios::trunc);
    if (!truncate) {
        throw std::runtime_error("unable to truncate WAL during snapshot import");
    }
    truncate.close();
    fsync_file(wal_path_, "unable to fsync truncated WAL during snapshot import");
    std::ofstream truncate_replication(replication_path_, std::ios::binary | std::ios::trunc);
    if (!truncate_replication) {
        throw std::runtime_error("unable to truncate replication journal during snapshot import");
    }
    truncate_replication.close();
    fsync_file(replication_path_, "unable to fsync truncated replication journal during snapshot import");
    values_ = std::move(snapshot_values);
    next_sequence_ = *snapshot_sequence + 1;
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
    fsync_file(wal_path_, "unable to fsync truncated WAL");
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

std::optional<std::uint64_t> KeyValueStore::load_sstable(
    const std::filesystem::path& path,
    std::unordered_map<std::string, ValueEntry>& loaded_values) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    char magic[sizeof(kSstableMagic) - 1]{};
    if (!input.read(magic, sizeof(magic))) {
        throw std::runtime_error("invalid SSTable header");
    }
    const bool current_format = std::equal(std::begin(magic), std::end(magic), std::begin(kSstableMagic));
    const bool ttl_format = std::equal(std::begin(magic), std::end(magic), std::begin(kTtlSstableMagic));
    const bool legacy_format = std::equal(std::begin(magic), std::end(magic), std::begin(kLegacySstableMagic));
    if (!current_format && !ttl_format && !legacy_format) {
        throw std::runtime_error("invalid SSTable header");
    }

    std::uint64_t snapshot_sequence = 0;
    if (current_format) {
        if (!read_u64(input, snapshot_sequence)) {
            throw std::runtime_error("truncated SSTable sequence");
        }
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
        if ((current_format || ttl_format) && !input.read(reinterpret_cast<char*>(&expires_at_ms), sizeof(expires_at_ms))) {
            throw std::runtime_error("truncated SSTable expiration");
        }
        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(value.data(), static_cast<std::streamsize>(value_size))) {
            throw std::runtime_error("truncated SSTable record");
        }
        loaded_values[std::move(key)] = ValueEntry{std::move(value), expires_at_ms};
    }
    return snapshot_sequence;
}

void KeyValueStore::replay_sstable() {
    std::unordered_map<std::string, ValueEntry> loaded_values;
    const auto snapshot_sequence = load_sstable(sstable_path_, loaded_values);
    if (!snapshot_sequence) {
        return;
    }
    for (auto& [key, value] : loaded_values) {
        values_[std::move(key)] = std::move(value);
    }
    next_sequence_ = std::max(next_sequence_, *snapshot_sequence + 1);
}

std::vector<KeyValueStore::JournalRecord> KeyValueStore::replay() {
    std::vector<JournalRecord> sequenced_records;
    std::ifstream input(wal_path_, std::ios::binary);
    if (!input) {
        return sequenced_records;
    }

    while (input.peek() != std::char_traits<char>::eof()) {
        const auto record_start = input.tellg();
        if (record_start < 0) {
            throw std::runtime_error("unable to read WAL offset");
        }
        char operation = 0;
        std::uint64_t sequence = 0;
        std::uint32_t key_size = 0;
        std::uint32_t value_size = 0;
        if (!input.get(operation)) {
            if (input.eof()) {
                input.close();
                truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL header");
                return sequenced_records;
            }
            throw std::runtime_error("truncated WAL header");
        }
        const auto sequenced_operation = from_sequenced_wal_operation(operation);
        if (sequenced_operation) {
            if (!read_u64(input, sequence) || !read_u32(input, key_size) || !read_u32(input, value_size)) {
                if (input.eof()) {
                    input.close();
                    truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL header");
                    return sequenced_records;
                }
                throw std::runtime_error("truncated WAL header");
            }
            if (sequence == 0) {
                throw std::runtime_error("invalid WAL sequence");
            }
            operation = *sequenced_operation;
        } else if (!read_u32(input, key_size) || !read_u32(input, value_size)) {
            if (input.eof()) {
                input.close();
                truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL header");
                return sequenced_records;
            }
            throw std::runtime_error("truncated WAL header");
        }
        if (key_size == 0 || key_size > 16 * 1024 * 1024 || value_size > kMaxRecordSize) {
            if (input.eof()) {
                input.close();
                truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL header");
                return sequenced_records;
            }
            throw std::runtime_error("invalid WAL record size");
        }

        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(value.data(), static_cast<std::streamsize>(value_size))) {
            if (input.eof()) {
                input.close();
                truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL record");
                return sequenced_records;
            }
            throw std::runtime_error("truncated WAL record");
        }
        if (sequence != 0) {
            std::uint32_t stored_checksum = 0;
            if (!read_u32(input, stored_checksum)) {
                if (input.eof()) {
                    input.close();
                    truncate_file_to(wal_path_, static_cast<std::uintmax_t>(record_start), "unable to truncate partial WAL checksum");
                    return sequenced_records;
                }
                throw std::runtime_error("truncated WAL checksum");
            }
            const auto expected_checksum = wal_record_checksum(sequence, operation, key, value);
            if (stored_checksum != expected_checksum) {
                throw std::runtime_error("WAL checksum mismatch");
            }
            next_sequence_ = std::max(next_sequence_, sequence + 1);
            sequenced_records.push_back(JournalRecord{sequence, operation, key, value});
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
    return sequenced_records;
}

std::unordered_set<std::uint64_t> KeyValueStore::replay_replication_index() {
    std::unordered_set<std::uint64_t> indexed_sequences;
    std::ifstream input(replication_path_, std::ios::binary);
    if (!input) {
        return indexed_sequences;
    }
    while (input.peek() != std::char_traits<char>::eof()) {
        const auto record_start = input.tellg();
        if (record_start < 0) {
            throw std::runtime_error("unable to read replication journal offset");
        }
        std::uint64_t sequence = 0;
        std::uint32_t key_size = 0;
        std::uint32_t payload_size = 0;
        char operation = 0;
        if (!read_u64(input, sequence) || !input.get(operation) ||
            !read_u32(input, key_size) || !read_u32(input, payload_size)) {
            if (input.eof()) {
                input.close();
                truncate_file_to(replication_path_, static_cast<std::uintmax_t>(record_start),
                                 "unable to truncate partial replication journal header");
                return indexed_sequences;
            }
            throw std::runtime_error("truncated replication journal header");
        }
        if (sequence == 0 || key_size == 0 || key_size > 16 * 1024 * 1024 || payload_size > kMaxRecordSize) {
            throw std::runtime_error("invalid replication journal record");
        }
        const auto bytes_to_skip = static_cast<std::streamsize>(key_size + payload_size);
        input.ignore(bytes_to_skip);
        if (input.gcount() != bytes_to_skip) {
            if (input.eof()) {
                input.close();
                truncate_file_to(replication_path_, static_cast<std::uintmax_t>(record_start),
                                 "unable to truncate partial replication journal record");
                return indexed_sequences;
            }
            throw std::runtime_error("truncated replication journal record");
        }
        indexed_sequences.insert(sequence);
        next_sequence_ = std::max(next_sequence_, sequence + 1);
    }
    return indexed_sequences;
}

void KeyValueStore::repair_replication_index(const std::vector<JournalRecord>& wal_records,
                                             std::unordered_set<std::uint64_t>& indexed_sequences) {
    for (const auto& record : wal_records) {
        if (indexed_sequences.find(record.sequence) != indexed_sequences.end()) {
            continue;
        }
        append_replication_record(record.sequence, record.operation, record.key, record.payload);
        indexed_sequences.insert(record.sequence);
    }
}

void KeyValueStore::append_replication_record(std::uint64_t sequence, char operation,
                                               const std::string& key, const std::string& payload) {
    std::ofstream output(replication_path_, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("unable to open replication journal: " + replication_path_.string());
    }
    write_u64(output, sequence);
    output.put(operation);
    write_u32(output, static_cast<std::uint32_t>(key.size()));
    write_u32(output, static_cast<std::uint32_t>(payload.size()));
    output.write(key.data(), static_cast<std::streamsize>(key.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("unable to append replication journal");
    }
    fsync_file(replication_path_, "unable to fsync replication journal");
}

std::string KeyValueStore::export_changes(std::uint64_t after_sequence) const {
    struct Change {
        std::uint64_t sequence;
        char operation;
        std::string key;
        std::string payload;
    };
    std::vector<Change> changes;
    std::ifstream input(replication_path_, std::ios::binary);
    if (input) {
        while (input.peek() != std::char_traits<char>::eof()) {
            Change change{};
            std::uint32_t key_size = 0;
            std::uint32_t payload_size = 0;
            if (!read_u64(input, change.sequence) || !input.get(change.operation) ||
                !read_u32(input, key_size) || !read_u32(input, payload_size)) {
                throw std::runtime_error("truncated replication journal header");
            }
            if (key_size == 0 || key_size > 16 * 1024 * 1024 || payload_size > kMaxRecordSize) {
                throw std::runtime_error("invalid replication journal record");
            }
            change.key.resize(key_size);
            change.payload.resize(payload_size);
            if (!input.read(change.key.data(), static_cast<std::streamsize>(key_size)) ||
                !input.read(change.payload.data(), static_cast<std::streamsize>(payload_size))) {
                throw std::runtime_error("truncated replication journal record");
            }
            if (change.sequence > after_sequence) {
                changes.push_back(std::move(change));
            }
        }
    }

    std::ostringstream output(std::ios::binary);
    output.write(kChangesMagic, sizeof(kChangesMagic) - 1);
    write_u32(output, static_cast<std::uint32_t>(changes.size()));
    for (const auto& change : changes) {
        write_u64(output, change.sequence);
        output.put(change.operation);
        write_u32(output, static_cast<std::uint32_t>(change.key.size()));
        write_u32(output, static_cast<std::uint32_t>(change.payload.size()));
        output.write(change.key.data(), static_cast<std::streamsize>(change.key.size()));
        output.write(change.payload.data(), static_cast<std::streamsize>(change.payload.size()));
    }
    return output.str();
}
void KeyValueStore::import_changes(const std::string& changes) {
    if (changes.size() < sizeof(kChangesMagic) - 1) {
        throw std::invalid_argument("invalid changes payload");
    }
    std::istringstream input(changes, std::ios::binary);
    char magic[sizeof(kChangesMagic) - 1]{};
    if (!input.read(magic, sizeof(magic)) ||
        !std::equal(std::begin(magic), std::end(magic), std::begin(kChangesMagic))) {
        throw std::invalid_argument("invalid changes header");
    }
    std::uint32_t count = 0;
    if (!read_u32(input, count) || count > 1000000) {
        throw std::invalid_argument("invalid changes count");
    }

    std::scoped_lock lock(mutex_);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t sequence = 0;
        char operation = 0;
        std::uint32_t key_size = 0;
        std::uint32_t payload_size = 0;
        if (!read_u64(input, sequence) || !input.get(operation) ||
            !read_u32(input, key_size) || !read_u32(input, payload_size) ||
            sequence == 0 || key_size == 0 || key_size > 16 * 1024 * 1024 || payload_size > kMaxRecordSize) {
            throw std::invalid_argument("invalid change record");
        }
        std::string key(key_size, '\0');
        std::string payload(payload_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(payload.data(), static_cast<std::streamsize>(payload_size))) {
            throw std::invalid_argument("truncated change record");
        }
        if (operation != kPut && operation != kPutWithTtl && operation != kDelete) {
            throw std::invalid_argument("unknown change operation");
        }
        if (sequence < next_sequence_) {
            continue;
        }
        if (sequence != next_sequence_) {
            throw std::invalid_argument("replication sequence gap");
        }
        append_record(sequence, operation == kPutWithTtl ? kPutWithTtl : operation, key, payload);
        append_replication_record(sequence, operation, key, payload);
        if (operation == kPut) {
            values_[key] = ValueEntry{payload, 0};
        } else if (operation == kPutWithTtl) {
            if (payload.size() < sizeof(std::int64_t)) {
                throw std::invalid_argument("invalid TTL change");
            }
            std::int64_t expires_at_ms = 0;
            std::memcpy(&expires_at_ms, payload.data(), sizeof(expires_at_ms));
            if (expires_at_ms > now_ms()) {
                values_[key] = ValueEntry{payload.substr(sizeof(expires_at_ms)), expires_at_ms};
            } else {
                values_.erase(key);
            }
        } else {
            values_.erase(key);
        }
        next_sequence_ = sequence + 1;
    }
}

std::uint64_t KeyValueStore::last_sequence() const {
    std::scoped_lock lock(mutex_);
    return next_sequence_ - 1;
}

std::vector<KeyValueStore::KeyInfo> KeyValueStore::list_keys() const {
    std::vector<KeyInfo> keys;
    std::scoped_lock lock(mutex_);
    const auto current_time_ms = now_ms();
    keys.reserve(values_.size());
    for (const auto& [key, value] : values_) {
        if (!is_expired(value, current_time_ms)) {
            keys.push_back(KeyInfo{key, value.value.size(), value.expires_at_ms});
        }
    }
    std::sort(keys.begin(), keys.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });
    return keys;
}

KeyValueStore::StorageStats KeyValueStore::storage_stats() const {
    std::scoped_lock lock(mutex_);
    std::error_code error;
    const auto file_size = [&error](const std::filesystem::path& path) {
        error.clear();
        const auto size = std::filesystem::file_size(path, error);
        return error ? static_cast<std::uintmax_t>(0) : size;
    };
    const auto current_time_ms = now_ms();
    const auto live_keys = static_cast<std::size_t>(std::count_if(values_.begin(), values_.end(), [current_time_ms](const auto& item) {
        return !is_expired(item.second, current_time_ms);
    }));
    return StorageStats{live_keys,
                        next_sequence_ - 1,
                        compactions_total_,
                        file_size(wal_path_),
                        file_size(sstable_path_),
                        file_size(replication_path_),
                        compaction_threshold_bytes_};
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
    write_u64(output, next_sequence_ - 1);
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
    fsync_file(temporary_path, "unable to fsync SSTable");
    std::filesystem::rename(temporary_path, sstable_path_);
    fsync_parent_directory(sstable_path_);
}

void KeyValueStore::append_record(std::uint64_t sequence, char operation, const std::string& key, const std::string& value) {
    const auto key_size = static_cast<std::uint32_t>(key.size());
    const auto value_size = static_cast<std::uint32_t>(value.size());
    wal_.put(to_wal_operation(operation));
    write_u64(wal_, sequence);
    write_u32(wal_, key_size);
    write_u32(wal_, value_size);
    wal_.write(key.data(), static_cast<std::streamsize>(key.size()));
    wal_.write(value.data(), static_cast<std::streamsize>(value.size()));
    const auto checksum = wal_record_checksum(sequence, operation, key, value);
    write_u32(wal_, checksum);
    wal_.flush();
    if (!wal_) {
        throw std::runtime_error("unable to append WAL record");
    }
    fsync_file(wal_path_, "unable to fsync WAL");
    request_compaction_if_needed();
}

}  // namespace stratadb
