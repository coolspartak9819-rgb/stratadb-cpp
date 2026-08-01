#include "storage/key_value_store.hpp"

#include <cassert>
#include <filesystem>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "stratadb-test.wal";
    std::filesystem::remove(path);
    {
        stratadb::KeyValueStore store(path);
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
        restored.compact();
    }
    {
        stratadb::KeyValueStore restored(path);
        assert(restored.get("binary") == "value with spaces");
        assert(restored.get("compacted") == "before compaction");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".sst");
}
