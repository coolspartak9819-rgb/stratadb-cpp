#include "storage/key_value_store.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace stratadb {
namespace {

constexpr char kPut = 'P';
constexpr char kDelete = 'D';

void write_u32(std::ostream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool read_u32(std::istream& input, std::uint32_t& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

}  // namespace

KeyValueStore::KeyValueStore(std::filesystem::path wal_path) : wal_path_(std::move(wal_path)) {
    if (wal_path_.has_parent_path()) {
        std::filesystem::create_directories(wal_path_.parent_path());
    }
    replay();
    wal_.open(wal_path_, std::ios::binary | std::ios::app);
    if (!wal_) {
        throw std::runtime_error("unable to open WAL: " + wal_path_.string());
    }
}

KeyValueStore::~KeyValueStore() {
    std::scoped_lock lock(mutex_);
    wal_.flush();
}

void KeyValueStore::put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        throw std::invalid_argument("key must not be empty");
    }
    std::scoped_lock lock(mutex_);
    append_record(kPut, key, value);
    values_[key] = value;
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    std::scoped_lock lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool KeyValueStore::erase(const std::string& key) {
    std::scoped_lock lock(mutex_);
    if (!values_.contains(key)) {
        return false;
    }
    append_record(kDelete, key, {});
    values_.erase(key);
    return true;
}

std::size_t KeyValueStore::size() const {
    std::scoped_lock lock(mutex_);
    return values_.size();
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
        if (key_size == 0 || key_size > 16 * 1024 * 1024 || value_size > 256 * 1024 * 1024) {
            throw std::runtime_error("invalid WAL record size");
        }

        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        if (!input.read(key.data(), static_cast<std::streamsize>(key_size)) ||
            !input.read(value.data(), static_cast<std::streamsize>(value_size))) {
            throw std::runtime_error("truncated WAL record");
        }
        if (operation == kPut) {
            values_[key] = std::move(value);
        } else if (operation == kDelete) {
            values_.erase(key);
        } else {
            throw std::runtime_error("unknown WAL operation");
        }
    }
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
}

}  // namespace stratadb
