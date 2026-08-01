#include "storage/key_value_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "stratadb-benchmark.wal";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
    std::filesystem::remove(path.string() + ".repl");

    constexpr int operations = 10000;
    const std::string value(256, 'x');
    stratadb::KeyValueStore store(path, std::numeric_limits<std::uintmax_t>::max());

    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < operations; ++index) {
        store.put("key-" + std::to_string(index), value);
    }
    const auto write_finished = std::chrono::steady_clock::now();

    for (int index = 0; index < operations; ++index) {
        if (!store.get("key-" + std::to_string(index))) {
            std::cerr << "benchmark read verification failed\n";
            return 1;
        }
    }
    const auto read_finished = std::chrono::steady_clock::now();
    const auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(write_finished - started).count();
    const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(read_finished - write_finished).count();

    std::cout << "operations=" << operations << " value_bytes=" << value.size() << '\n'
              << "write_ms=" << write_ms << " write_ops_per_sec=" << (operations * 1000.0 / std::max<std::int64_t>(1, write_ms)) << '\n'
              << "read_ms=" << read_ms << " read_ops_per_sec=" << (operations * 1000.0 / std::max<std::int64_t>(1, read_ms)) << '\n';

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
    std::filesystem::remove(path.string() + ".repl");
}
