#include "storage/key_value_store.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "stratadb-test.wal";
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
        stratadb::KeyValueStore restored(path);
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
        restored.put("short-ttl", "temporary", std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(!restored.get("short-ttl"));

        const auto follower_path = std::filesystem::temp_directory_path() / "stratadb-follower.wal";
        std::filesystem::remove(follower_path);
        std::filesystem::remove(follower_path.string() + ".sst");
        std::filesystem::remove(follower_path.string() + ".repl");
        stratadb::KeyValueStore follower(follower_path);
        follower.import_snapshot(restored.export_snapshot());
        assert(follower.get("binary") == "value with spaces");
        assert(follower.get("persistent-ttl") == "still alive");
        restored.put("incremental", "from journal");
        const auto changes = restored.export_changes(follower.last_sequence());
        follower.import_changes(changes);
        follower.import_changes(changes);
        assert(follower.get("incremental") == "from journal");
        std::filesystem::remove(follower_path);
        std::filesystem::remove(follower_path.string() + ".sst");
        std::filesystem::remove(follower_path.string() + ".repl");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
    std::filesystem::remove(path.string() + ".repl");
}
