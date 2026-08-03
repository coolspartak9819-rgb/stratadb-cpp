#include "storage/key_value_store.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void append_u32(std::ostream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_u64(std::ostream& output, std::uint64_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_partial_wal_header(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.put('P');
    append_u32(output, 7);
}

void append_partial_wal_body(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.put('P');
    append_u32(output, 7);
    append_u32(output, 5);
    output.write("crashed", 7);
    output.write("pa", 2);
}

void append_partial_replication_header(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.put('\2');
    output.put('\0');
}

void append_partial_replication_body(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    append_u64(output, 2);
    output.put('P');
    append_u32(output, 7);
    append_u32(output, 5);
    output.write("missing", 7);
    output.write("pa", 2);
}

void flip_last_byte(const std::filesystem::path& path) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x01);
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
}

std::filesystem::path test_path(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("stratadb-" + name + "-" + std::to_string(::getpid()) + ".wal");
}

}  // namespace

int main() {
    const auto path = test_path("store");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
    std::filesystem::remove(path.string() + ".repl");
    {
        stratadb::KeyValueStore store(path, 128);
        store.put("name", "strata");
        store.put("name", "stratadb");
        assert(store.get("name") == "stratadb");
        assert(store.size() == 1);
        assert(store.erase("name"));
        assert(!store.get("name"));
    }
    {
        stratadb::KeyValueStore restored(path, 128);
        assert(!restored.get("name"));
        restored.put("binary", "value with spaces");
        restored.put("compacted", "before compaction");
        restored.put("persistent-ttl", "still alive", std::chrono::seconds(60));
        restored.compact();
        const auto compactions_before = restored.compactions_total();
        restored.put("background", std::string(256, 'x'));
        for (int attempt = 0; attempt < 100 && restored.compactions_total() == compactions_before; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(restored.compactions_total() > compactions_before);
    }
    {
        stratadb::KeyValueStore restored(path);
        assert(restored.get("binary") == "value with spaces");
        assert(restored.get("compacted") == "before compaction");
        assert(restored.get("background") == std::string(256, 'x'));
        assert(restored.get("persistent-ttl") == "still alive");
        const auto listed_keys = restored.list_keys();
        assert(listed_keys.size() == 4);
        assert(listed_keys.front().key == "background");
        const auto stats = restored.storage_stats();
        assert(stats.keys == 4);
        assert(stats.last_sequence == restored.last_sequence());
        assert(stats.sstable_bytes > 0);
        assert(stats.replication_bytes > 0);
        assert(stats.compaction_threshold_bytes > 0);
        restored.put("short-ttl", "temporary", std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(!restored.get("short-ttl"));

        const auto follower_path = test_path("follower");
        std::filesystem::remove(follower_path);
        std::filesystem::remove(follower_path.string() + ".sst");
        std::filesystem::remove(follower_path.string() + ".repl");
        stratadb::KeyValueStore follower(follower_path);
        const auto snapshot_sequence = restored.last_sequence();
        follower.import_snapshot(restored.export_snapshot());
        assert(follower.last_sequence() == snapshot_sequence);
        assert(follower.get("binary") == "value with spaces");
        assert(follower.get("persistent-ttl") == "still alive");
        bool rejected_snapshot = false;
        try {
            follower.import_snapshot("not an sstable");
        } catch (const std::exception&) {
            rejected_snapshot = true;
        }
        assert(rejected_snapshot);
        assert(follower.last_sequence() == snapshot_sequence);
        assert(follower.get("binary") == "value with spaces");
        assert(!std::filesystem::exists(follower_path.string() + ".sst.incoming"));
        follower.import_changes(restored.export_changes(follower.last_sequence()));
        assert(follower.last_sequence() == snapshot_sequence);
        restored.put("incremental", "from journal");
        const auto changes = restored.export_changes(follower.last_sequence());
        follower.import_changes(changes);
        assert(follower.last_sequence() == restored.last_sequence());
        const auto replication_journal_size = std::filesystem::file_size(follower_path.string() + ".repl");
        follower.import_changes(changes);
        assert(follower.last_sequence() == restored.last_sequence());
        assert(std::filesystem::file_size(follower_path.string() + ".repl") == replication_journal_size);
        assert(follower.get("incremental") == "from journal");

        const auto gap_path = test_path("gap-follower");
        std::filesystem::remove(gap_path);
        std::filesystem::remove(gap_path.string() + ".sst");
        std::filesystem::remove(gap_path.string() + ".repl");
        stratadb::KeyValueStore gap_follower(gap_path);
        bool rejected_gap = false;
        try {
            gap_follower.import_changes(changes);
        } catch (const std::invalid_argument&) {
            rejected_gap = true;
        }
        assert(rejected_gap);
        std::filesystem::remove(gap_path);
        std::filesystem::remove(gap_path.string() + ".sst");
        std::filesystem::remove(gap_path.string() + ".repl");
        std::filesystem::remove(follower_path);
        std::filesystem::remove(follower_path.string() + ".sst");
        std::filesystem::remove(follower_path.string() + ".repl");
    }
    const auto partial_header_path = test_path("partial-header");
    std::filesystem::remove(partial_header_path);
    std::filesystem::remove(partial_header_path.string() + ".sst");
    std::filesystem::remove(partial_header_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(partial_header_path);
        store.put("stable", "value");
    }
    const auto partial_header_good_size = std::filesystem::file_size(partial_header_path);
    append_partial_wal_header(partial_header_path);
    assert(std::filesystem::file_size(partial_header_path) > partial_header_good_size);
    {
        stratadb::KeyValueStore restored(partial_header_path);
        assert(restored.get("stable") == "value");
    }
    assert(std::filesystem::file_size(partial_header_path) == partial_header_good_size);
    std::filesystem::remove(partial_header_path);
    std::filesystem::remove(partial_header_path.string() + ".sst");
    std::filesystem::remove(partial_header_path.string() + ".repl");

    const auto partial_body_path = test_path("partial-body");
    std::filesystem::remove(partial_body_path);
    std::filesystem::remove(partial_body_path.string() + ".sst");
    std::filesystem::remove(partial_body_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(partial_body_path);
        store.put("stable", "value");
    }
    const auto partial_body_good_size = std::filesystem::file_size(partial_body_path);
    append_partial_wal_body(partial_body_path);
    assert(std::filesystem::file_size(partial_body_path) > partial_body_good_size);
    {
        stratadb::KeyValueStore restored(partial_body_path);
        assert(restored.get("stable") == "value");
        assert(!restored.get("crashed"));
    }
    assert(std::filesystem::file_size(partial_body_path) == partial_body_good_size);
    std::filesystem::remove(partial_body_path);
    std::filesystem::remove(partial_body_path.string() + ".sst");
    std::filesystem::remove(partial_body_path.string() + ".repl");

    const auto corrupted_wal_path = test_path("corrupted-checksum");
    std::filesystem::remove(corrupted_wal_path);
    std::filesystem::remove(corrupted_wal_path.string() + ".sst");
    std::filesystem::remove(corrupted_wal_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(corrupted_wal_path);
        store.put("stable", "value");
    }
    flip_last_byte(corrupted_wal_path);
    bool rejected_corruption = false;
    try {
        stratadb::KeyValueStore restored(corrupted_wal_path);
    } catch (const std::runtime_error&) {
        rejected_corruption = true;
    }
    assert(rejected_corruption);
    std::filesystem::remove(corrupted_wal_path);
    std::filesystem::remove(corrupted_wal_path.string() + ".sst");
    std::filesystem::remove(corrupted_wal_path.string() + ".repl");

    const auto partial_replication_header_path = test_path("partial-repl-header");
    std::filesystem::remove(partial_replication_header_path);
    std::filesystem::remove(partial_replication_header_path.string() + ".sst");
    std::filesystem::remove(partial_replication_header_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(partial_replication_header_path);
        store.put("stable", "value");
    }
    const auto partial_replication_header_good_size =
        std::filesystem::file_size(partial_replication_header_path.string() + ".repl");
    append_partial_replication_header(partial_replication_header_path.string() + ".repl");
    assert(std::filesystem::file_size(partial_replication_header_path.string() + ".repl") >
           partial_replication_header_good_size);
    {
        stratadb::KeyValueStore restored(partial_replication_header_path);
        assert(restored.get("stable") == "value");
        assert(restored.last_sequence() == 1);
    }
    assert(std::filesystem::file_size(partial_replication_header_path.string() + ".repl") ==
           partial_replication_header_good_size);
    std::filesystem::remove(partial_replication_header_path);
    std::filesystem::remove(partial_replication_header_path.string() + ".sst");
    std::filesystem::remove(partial_replication_header_path.string() + ".repl");

    const auto partial_replication_body_path = test_path("partial-repl-body");
    std::filesystem::remove(partial_replication_body_path);
    std::filesystem::remove(partial_replication_body_path.string() + ".sst");
    std::filesystem::remove(partial_replication_body_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(partial_replication_body_path);
        store.put("stable", "value");
    }
    const auto partial_replication_body_good_size =
        std::filesystem::file_size(partial_replication_body_path.string() + ".repl");
    append_partial_replication_body(partial_replication_body_path.string() + ".repl");
    assert(std::filesystem::file_size(partial_replication_body_path.string() + ".repl") >
           partial_replication_body_good_size);
    {
        stratadb::KeyValueStore restored(partial_replication_body_path);
        assert(restored.get("stable") == "value");
        assert(!restored.get("missing"));
        assert(restored.last_sequence() == 1);
    }
    assert(std::filesystem::file_size(partial_replication_body_path.string() + ".repl") ==
           partial_replication_body_good_size);
    std::filesystem::remove(partial_replication_body_path);
    std::filesystem::remove(partial_replication_body_path.string() + ".sst");
    std::filesystem::remove(partial_replication_body_path.string() + ".repl");

    const auto repaired_replication_path = test_path("repaired-repl");
    const auto repaired_follower_path = test_path("repaired-repl-follower");
    std::filesystem::remove(repaired_replication_path);
    std::filesystem::remove(repaired_replication_path.string() + ".sst");
    std::filesystem::remove(repaired_replication_path.string() + ".repl");
    std::filesystem::remove(repaired_follower_path);
    std::filesystem::remove(repaired_follower_path.string() + ".sst");
    std::filesystem::remove(repaired_follower_path.string() + ".repl");
    {
        stratadb::KeyValueStore store(repaired_replication_path);
        store.put("repairable", "from sequenced WAL");
    }
    std::filesystem::remove(repaired_replication_path.string() + ".repl");
    {
        stratadb::KeyValueStore restored(repaired_replication_path);
        assert(restored.get("repairable") == "from sequenced WAL");
        assert(restored.last_sequence() == 1);
        assert(std::filesystem::exists(repaired_replication_path.string() + ".repl"));
        stratadb::KeyValueStore follower(repaired_follower_path);
        follower.import_changes(restored.export_changes(0));
        assert(follower.get("repairable") == "from sequenced WAL");
    }
    std::filesystem::remove(repaired_replication_path);
    std::filesystem::remove(repaired_replication_path.string() + ".sst");
    std::filesystem::remove(repaired_replication_path.string() + ".repl");
    std::filesystem::remove(repaired_follower_path);
    std::filesystem::remove(repaired_follower_path.string() + ".sst");
    std::filesystem::remove(repaired_follower_path.string() + ".repl");

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
    std::filesystem::remove(path.string() + ".repl");
}
