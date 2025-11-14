#include <cassert>
#include <iostream>
#include <string>
#include <algorithm>
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/exceptions.hpp"

// Advanced tests for resources functionality
// Tests multiple resource types, metadata handling, and edge cases

using namespace fastmcpp;

void test_multiple_resource_kinds() {
    std::cout << "Test 1: Multiple resource kinds...\n";

    resources::ResourceManager rm;

    // File resource
    resources::Resource file_res{
        Id{"file1"},
        resources::Kind::File,
        Json{{"path", "/data/file.txt"}, {"size", 1024}}
    };

    // Text resource
    resources::Resource text_res{
        Id{"text1"},
        resources::Kind::Text,
        Json{{"content", "Hello World"}, {"encoding", "utf-8"}}
    };

    // JSON resource
    resources::Resource json_res{
        Id{"json1"},
        resources::Kind::Json,
        Json{{"data", Json{{"key", "value"}}}}
    };

    // Unknown kind resource
    resources::Resource unknown_res{
        Id{"unknown1"},
        resources::Kind::Unknown,
        Json::object()
    };

    rm.register_resource(file_res);
    rm.register_resource(text_res);
    rm.register_resource(json_res);
    rm.register_resource(unknown_res);

    // Verify all registered
    auto list = rm.list();
    assert(list.size() == 4);

    // Verify kinds are preserved
    auto retrieved_file = rm.get("file1");
    assert(retrieved_file.kind == resources::Kind::File);
    assert(retrieved_file.metadata["path"] == "/data/file.txt");

    auto retrieved_text = rm.get("text1");
    assert(retrieved_text.kind == resources::Kind::Text);
    assert(retrieved_text.metadata["content"] == "Hello World");

    auto retrieved_json = rm.get("json1");
    assert(retrieved_json.kind == resources::Kind::Json);
    assert(retrieved_json.metadata["data"]["key"] == "value");

    auto retrieved_unknown = rm.get("unknown1");
    assert(retrieved_unknown.kind == resources::Kind::Unknown);

    std::cout << "  ✓ Multiple resource kinds work correctly\n";
}

void test_resource_metadata() {
    std::cout << "Test 2: Resource metadata handling...\n";

    resources::ResourceManager rm;

    // Resource with rich metadata
    resources::Resource rich_res{
        Id{"rich1"},
        resources::Kind::File,
        Json{
            {"name", "document.pdf"},
            {"size_bytes", 2048},
            {"created_at", "2025-01-01T00:00:00Z"},
            {"tags", Json::array({"important", "draft"})},
            {"author", Json{
                {"name", "Alice"},
                {"email", "alice@example.com"}
            }}
        }
    };

    rm.register_resource(rich_res);

    auto retrieved = rm.get("rich1");
    assert(retrieved.metadata["name"] == "document.pdf");
    assert(retrieved.metadata["size_bytes"] == 2048);
    assert(retrieved.metadata["tags"].size() == 2);
    assert(retrieved.metadata["author"]["name"] == "Alice");

    std::cout << "  ✓ Rich metadata preserved correctly\n";
}

void test_resource_update() {
    std::cout << "Test 3: Resource update/replacement...\n";

    resources::ResourceManager rm;

    // Register initial version
    resources::Resource v1{
        Id{"doc1"},
        resources::Kind::Text,
        Json{{"version", 1}, {"content", "Version 1"}}
    };

    rm.register_resource(v1);
    assert(rm.get("doc1").metadata["version"] == 1);

    // Update with new version (same ID)
    resources::Resource v2{
        Id{"doc1"},
        resources::Kind::Text,
        Json{{"version", 2}, {"content", "Version 2"}}
    };

    rm.register_resource(v2);

    // Should replace, not duplicate
    auto list = rm.list();
    assert(list.size() == 1);

    auto current = rm.get("doc1");
    assert(current.metadata["version"] == 2);
    assert(current.metadata["content"] == "Version 2");

    std::cout << "  ✓ Resource replacement works correctly\n";
}

void test_resource_not_found() {
    std::cout << "Test 4: Resource not found error...\n";

    resources::ResourceManager rm;

    bool threw = false;
    try {
        rm.get("nonexistent");
    } catch (const NotFoundError& e) {
        threw = true;
    }
    assert(threw);

    std::cout << "  ✓ NotFoundError thrown for missing resources\n";
}

void test_resource_list_ordering() {
    std::cout << "Test 5: Resource list operations...\n";

    resources::ResourceManager rm;

    // Initially empty
    assert(rm.list().empty());

    // Add multiple resources
    for (int i = 0; i < 5; ++i) {
        resources::Resource res{
            Id{"res_" + std::to_string(i)},
            resources::Kind::Text,
            Json{{"index", i}}
        };
        rm.register_resource(res);
    }

    auto list = rm.list();
    assert(list.size() == 5);

    // Verify all resources present
    for (int i = 0; i < 5; ++i) {
        std::string id = "res_" + std::to_string(i);
        auto found = std::find_if(list.begin(), list.end(),
            [&id](const resources::Resource& r) { return r.id.value == id; });
        assert(found != list.end());
        assert(found->metadata["index"] == i);
    }

    std::cout << "  ✓ Resource listing works correctly\n";
}

void test_empty_metadata() {
    std::cout << "Test 6: Empty and minimal metadata...\n";

    resources::ResourceManager rm;

    // Resource with empty metadata
    resources::Resource empty_meta{
        Id{"empty1"},
        resources::Kind::Text,
        Json::object()
    };

    rm.register_resource(empty_meta);

    auto retrieved = rm.get("empty1");
    assert(retrieved.metadata.empty());
    assert(retrieved.kind == resources::Kind::Text);

    std::cout << "  ✓ Empty metadata handled correctly\n";
}

void test_large_metadata() {
    std::cout << "Test 7: Large metadata objects...\n";

    resources::ResourceManager rm;

    // Resource with large metadata
    Json large_meta;
    for (int i = 0; i < 100; ++i) {
        large_meta["field_" + std::to_string(i)] = "value_" + std::to_string(i);
    }

    resources::Resource large_res{
        Id{"large1"},
        resources::Kind::Json,
        large_meta
    };

    rm.register_resource(large_res);

    auto retrieved = rm.get("large1");
    assert(retrieved.metadata.size() == 100);
    assert(retrieved.metadata["field_50"] == "value_50");

    std::cout << "  ✓ Large metadata handled correctly\n";
}

void test_special_characters_in_id() {
    std::cout << "Test 8: Special characters in resource IDs...\n";

    resources::ResourceManager rm;

    // IDs with special characters
    std::vector<std::string> special_ids = {
        "res:with:colons",
        "res/with/slashes",
        "res.with.dots",
        "res-with-dashes",
        "res_with_underscores",
        "res@with@at",
        "res#with#hash"
    };

    for (const auto& id : special_ids) {
        resources::Resource res{
            Id{id},
            resources::Kind::Text,
            Json{{"id", id}}
        };
        rm.register_resource(res);
    }

    // Verify all can be retrieved
    for (const auto& id : special_ids) {
        auto retrieved = rm.get(id);
        assert(retrieved.id.value == id);
        assert(retrieved.metadata["id"] == id);
    }

    assert(rm.list().size() == special_ids.size());

    std::cout << "  ✓ Special characters in IDs handled correctly\n";
}

void test_kind_string_conversion() {
    std::cout << "Test 9: Kind to string conversion...\n";

    assert(std::string(resources::to_string(resources::Kind::File)) == "file");
    assert(std::string(resources::to_string(resources::Kind::Text)) == "text");
    assert(std::string(resources::to_string(resources::Kind::Json)) == "json");
    assert(std::string(resources::to_string(resources::Kind::Unknown)) == "unknown");

    std::cout << "  ✓ Kind string conversion works correctly\n";
}

void test_many_resources() {
    std::cout << "Test 10: Managing many resources...\n";

    resources::ResourceManager rm;

    const int num_resources = 100;

    // Register many resources
    for (int i = 0; i < num_resources; ++i) {
        resources::Resource res{
            Id{"bulk_" + std::to_string(i)},
            static_cast<resources::Kind>((i % 4)), // Cycle through kinds
            Json{{"index", i}}
        };
        rm.register_resource(res);
    }

    auto list = rm.list();
    assert(list.size() == num_resources);

    // Verify random access
    auto res_50 = rm.get("bulk_50");
    assert(res_50.metadata["index"] == 50);

    auto res_99 = rm.get("bulk_99");
    assert(res_99.metadata["index"] == 99);

    std::cout << "  ✓ Many resources managed correctly\n";
}

int main() {
    std::cout << "Running advanced resources tests...\n\n";

    try {
        test_multiple_resource_kinds();
        test_resource_metadata();
        test_resource_update();
        test_resource_not_found();
        test_resource_list_ordering();
        test_empty_metadata();
        test_large_metadata();
        test_special_characters_in_id();
        test_kind_string_conversion();
        test_many_resources();

        std::cout << "\n✅ All advanced resources tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
