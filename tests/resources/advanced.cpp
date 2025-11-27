#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/manager.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

// Advanced tests for resources functionality
// Tests multiple resource types, metadata handling, edge cases,
// and client-side resource type serialization

using namespace fastmcpp;

void test_multiple_resource_kinds()
{
    std::cout << "Test 1: Multiple resource kinds...\n";

    resources::ResourceManager rm;

    // File resource
    resources::Resource file_res{Id{"file1"}, resources::Kind::File,
                                 Json{{"path", "/data/file.txt"}, {"size", 1024}}};

    // Text resource
    resources::Resource text_res{Id{"text1"}, resources::Kind::Text,
                                 Json{{"content", "Hello World"}, {"encoding", "utf-8"}}};

    // JSON resource
    resources::Resource json_res{Id{"json1"}, resources::Kind::Json,
                                 Json{{"data", Json{{"key", "value"}}}}};

    // Unknown kind resource
    resources::Resource unknown_res{Id{"unknown1"}, resources::Kind::Unknown, Json::object()};

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

    std::cout << "  [PASS] Multiple resource kinds work correctly\n";
}

void test_resource_metadata()
{
    std::cout << "Test 2: Resource metadata handling...\n";

    resources::ResourceManager rm;

    // Resource with rich metadata
    resources::Resource rich_res{
        Id{"rich1"}, resources::Kind::File,
        Json{{"name", "document.pdf"},
             {"size_bytes", 2048},
             {"created_at", "2025-01-01T00:00:00Z"},
             {"tags", Json::array({"important", "draft"})},
             {"author", Json{{"name", "Alice"}, {"email", "alice@example.com"}}}}};

    rm.register_resource(rich_res);

    auto retrieved = rm.get("rich1");
    assert(retrieved.metadata["name"] == "document.pdf");
    assert(retrieved.metadata["size_bytes"] == 2048);
    assert(retrieved.metadata["tags"].size() == 2);
    assert(retrieved.metadata["author"]["name"] == "Alice");

    std::cout << "  [PASS] Rich metadata preserved correctly\n";
}

void test_resource_update()
{
    std::cout << "Test 3: Resource update/replacement...\n";

    resources::ResourceManager rm;

    // Register initial version
    resources::Resource v1{Id{"doc1"}, resources::Kind::Text,
                           Json{{"version", 1}, {"content", "Version 1"}}};

    rm.register_resource(v1);
    assert(rm.get("doc1").metadata["version"] == 1);

    // Update with new version (same ID)
    resources::Resource v2{Id{"doc1"}, resources::Kind::Text,
                           Json{{"version", 2}, {"content", "Version 2"}}};

    rm.register_resource(v2);

    // Should replace, not duplicate
    auto list = rm.list();
    assert(list.size() == 1);

    auto current = rm.get("doc1");
    assert(current.metadata["version"] == 2);
    assert(current.metadata["content"] == "Version 2");

    std::cout << "  [PASS] Resource replacement works correctly\n";
}

void test_resource_not_found()
{
    std::cout << "Test 4: Resource not found error...\n";

    resources::ResourceManager rm;

    bool threw = false;
    try
    {
        rm.get("nonexistent");
    }
    catch (const NotFoundError& e)
    {
        threw = true;
    }
    assert(threw);

    std::cout << "  [PASS] NotFoundError thrown for missing resources\n";
}

void test_resource_list_ordering()
{
    std::cout << "Test 5: Resource list operations...\n";

    resources::ResourceManager rm;

    // Initially empty
    assert(rm.list().empty());

    // Add multiple resources
    for (int i = 0; i < 5; ++i)
    {
        resources::Resource res{Id{"res_" + std::to_string(i)}, resources::Kind::Text,
                                Json{{"index", i}}};
        rm.register_resource(res);
    }

    auto list = rm.list();
    assert(list.size() == 5);

    // Verify all resources present
    for (int i = 0; i < 5; ++i)
    {
        std::string id = "res_" + std::to_string(i);
        auto found = std::find_if(list.begin(), list.end(),
                                  [&id](const resources::Resource& r) { return r.id.value == id; });
        assert(found != list.end());
        assert(found->metadata["index"] == i);
    }

    std::cout << "  [PASS] Resource listing works correctly\n";
}

void test_empty_metadata()
{
    std::cout << "Test 6: Empty and minimal metadata...\n";

    resources::ResourceManager rm;

    // Resource with empty metadata
    resources::Resource empty_meta{Id{"empty1"}, resources::Kind::Text, Json::object()};

    rm.register_resource(empty_meta);

    auto retrieved = rm.get("empty1");
    assert(retrieved.metadata.empty());
    assert(retrieved.kind == resources::Kind::Text);

    std::cout << "  [PASS] Empty metadata handled correctly\n";
}

void test_large_metadata()
{
    std::cout << "Test 7: Large metadata objects...\n";

    resources::ResourceManager rm;

    // Resource with large metadata
    Json large_meta;
    for (int i = 0; i < 100; ++i)
        large_meta["field_" + std::to_string(i)] = "value_" + std::to_string(i);

    resources::Resource large_res{Id{"large1"}, resources::Kind::Json, large_meta};

    rm.register_resource(large_res);

    auto retrieved = rm.get("large1");
    assert(retrieved.metadata.size() == 100);
    assert(retrieved.metadata["field_50"] == "value_50");

    std::cout << "  [PASS] Large metadata handled correctly\n";
}

void test_special_characters_in_id()
{
    std::cout << "Test 8: Special characters in resource IDs...\n";

    resources::ResourceManager rm;

    // IDs with special characters
    std::vector<std::string> special_ids = {
        "res:with:colons",      "res/with/slashes", "res.with.dots", "res-with-dashes",
        "res_with_underscores", "res@with@at",      "res#with#hash"};

    for (const auto& id : special_ids)
    {
        resources::Resource res{Id{id}, resources::Kind::Text, Json{{"id", id}}};
        rm.register_resource(res);
    }

    // Verify all can be retrieved
    for (const auto& id : special_ids)
    {
        auto retrieved = rm.get(id);
        assert(retrieved.id.value == id);
        assert(retrieved.metadata["id"] == id);
    }

    assert(rm.list().size() == special_ids.size());

    std::cout << "  [PASS] Special characters in IDs handled correctly\n";
}

void test_kind_string_conversion()
{
    std::cout << "Test 9: Kind to string conversion...\n";

    assert(std::string(resources::to_string(resources::Kind::File)) == "file");
    assert(std::string(resources::to_string(resources::Kind::Text)) == "text");
    assert(std::string(resources::to_string(resources::Kind::Json)) == "json");
    assert(std::string(resources::to_string(resources::Kind::Unknown)) == "unknown");

    std::cout << "  [PASS] Kind string conversion works correctly\n";
}

void test_many_resources()
{
    std::cout << "Test 10: Managing many resources...\n";

    resources::ResourceManager rm;

    const int num_resources = 100;

    // Register many resources
    for (int i = 0; i < num_resources; ++i)
    {
        resources::Resource res{Id{"bulk_" + std::to_string(i)},
                                static_cast<resources::Kind>((i % 4)), // Cycle through kinds
                                Json{{"index", i}}};
        rm.register_resource(res);
    }

    auto list = rm.list();
    assert(list.size() == num_resources);

    // Verify random access
    auto res_50 = rm.get("bulk_50");
    assert(res_50.metadata["index"] == 50);

    auto res_99 = rm.get("bulk_99");
    assert(res_99.metadata["index"] == 99);

    std::cout << "  [PASS] Many resources managed correctly\n";
}

// ============================================================================
// Client-side Resource Type Tests
// ============================================================================

void test_resource_info_serialization()
{
    std::cout << "Test 11: ResourceInfo JSON serialization...\n";

    // Create ResourceInfo with all fields
    client::ResourceInfo info;
    info.uri = "file:///data/doc.txt";
    info.name = "Document";
    info.description = "A test document";
    info.mimeType = "text/plain";
    info.annotations = Json{{"author", "Alice"}, {"version", 1}};

    // Serialize to JSON
    Json j;
    to_json(j, info);

    assert(j["uri"] == "file:///data/doc.txt");
    assert(j["name"] == "Document");
    assert(j["description"] == "A test document");
    assert(j["mimeType"] == "text/plain");
    assert(j["annotations"]["author"] == "Alice");

    // Deserialize back
    client::ResourceInfo parsed;
    from_json(j, parsed);

    assert(parsed.uri == info.uri);
    assert(parsed.name == info.name);
    assert(parsed.description == info.description);
    assert(parsed.mimeType == info.mimeType);
    assert(parsed.annotations.value()["version"] == 1);

    std::cout << "  [PASS] ResourceInfo serialization works correctly\n";
}

void test_resource_info_minimal()
{
    std::cout << "Test 12: ResourceInfo with minimal fields...\n";

    // Only required fields
    Json j = {{"uri", "mem://test"}, {"name", "test"}};

    client::ResourceInfo info;
    from_json(j, info);

    assert(info.uri == "mem://test");
    assert(info.name == "test");
    assert(!info.description.has_value());
    assert(!info.mimeType.has_value());
    assert(!info.annotations.has_value());

    std::cout << "  [PASS] Minimal ResourceInfo parsed correctly\n";
}

void test_resource_template_fields()
{
    std::cout << "Test 13: ResourceTemplate fields...\n";

    client::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///data/{filename}";
    tmpl.name = "File Template";
    tmpl.description = "Access files by name";
    tmpl.mimeType = "application/octet-stream";

    assert(tmpl.uriTemplate == "file:///data/{filename}");
    assert(tmpl.name == "File Template");
    assert(tmpl.description.value() == "Access files by name");
    assert(tmpl.mimeType.value() == "application/octet-stream");

    std::cout << "  [PASS] ResourceTemplate fields work correctly\n";
}

void test_text_resource_content_parsing()
{
    std::cout << "Test 14: TextResourceContent parsing...\n";

    Json j = {{"uri", "file:///readme.md"},
              {"mimeType", "text/markdown"},
              {"text", "# Hello World\n\nThis is a test."}};

    client::TextResourceContent content;
    from_json(j, content);

    assert(content.uri == "file:///readme.md");
    assert(content.mimeType.value() == "text/markdown");
    assert(content.text == "# Hello World\n\nThis is a test.");

    std::cout << "  [PASS] TextResourceContent parsed correctly\n";
}

void test_blob_resource_content_parsing()
{
    std::cout << "Test 15: BlobResourceContent parsing...\n";

    // Base64 encoded "Hello"
    std::string base64_data = "SGVsbG8=";

    Json j = {{"uri", "file:///image.png"}, {"mimeType", "image/png"}, {"blob", base64_data}};

    client::BlobResourceContent content;
    from_json(j, content);

    assert(content.uri == "file:///image.png");
    assert(content.mimeType.value() == "image/png");
    assert(content.blob == base64_data);

    std::cout << "  [PASS] BlobResourceContent parsed correctly\n";
}

void test_parse_resource_content_text()
{
    std::cout << "Test 16: parse_resource_content for text...\n";

    Json j = {{"uri", "mem://doc"}, {"text", "Document content"}};

    auto content = client::parse_resource_content(j);

    // Should be TextResourceContent
    assert(std::holds_alternative<client::TextResourceContent>(content));
    auto& text = std::get<client::TextResourceContent>(content);
    assert(text.uri == "mem://doc");
    assert(text.text == "Document content");

    std::cout << "  [PASS] Text content parsed via parse_resource_content\n";
}

void test_parse_resource_content_blob()
{
    std::cout << "Test 17: parse_resource_content for blob...\n";

    Json j = {{"uri", "file:///binary.dat"},
              {"blob", "AQIDBA=="}, // Base64 for bytes 1,2,3,4
              {"mimeType", "application/octet-stream"}};

    auto content = client::parse_resource_content(j);

    // Should be BlobResourceContent
    assert(std::holds_alternative<client::BlobResourceContent>(content));
    auto& blob = std::get<client::BlobResourceContent>(content);
    assert(blob.uri == "file:///binary.dat");
    assert(blob.blob == "AQIDBA==");
    assert(blob.mimeType.value() == "application/octet-stream");

    std::cout << "  [PASS] Blob content parsed via parse_resource_content\n";
}

void test_list_resources_result()
{
    std::cout << "Test 18: ListResourcesResult structure...\n";

    client::ListResourcesResult result;

    // Add multiple resources
    client::ResourceInfo r1;
    r1.uri = "file:///a.txt";
    r1.name = "File A";

    client::ResourceInfo r2;
    r2.uri = "file:///b.txt";
    r2.name = "File B";
    r2.description = "Second file";

    result.resources.push_back(r1);
    result.resources.push_back(r2);
    result.nextCursor = "cursor_abc";

    assert(result.resources.size() == 2);
    assert(result.resources[0].name == "File A");
    assert(result.resources[1].description.value() == "Second file");
    assert(result.nextCursor.value() == "cursor_abc");

    std::cout << "  [PASS] ListResourcesResult works correctly\n";
}

void test_list_resource_templates_result()
{
    std::cout << "Test 19: ListResourceTemplatesResult structure...\n";

    client::ListResourceTemplatesResult result;

    client::ResourceTemplate t1;
    t1.uriTemplate = "db://{table}/{id}";
    t1.name = "Database Record";

    client::ResourceTemplate t2;
    t2.uriTemplate = "api://{endpoint}";
    t2.name = "API Endpoint";
    t2.mimeType = "application/json";

    result.resourceTemplates.push_back(t1);
    result.resourceTemplates.push_back(t2);

    assert(result.resourceTemplates.size() == 2);
    assert(result.resourceTemplates[0].uriTemplate == "db://{table}/{id}");
    assert(result.resourceTemplates[1].mimeType.value() == "application/json");

    std::cout << "  [PASS] ListResourceTemplatesResult works correctly\n";
}

void test_read_resource_result()
{
    std::cout << "Test 20: ReadResourceResult with multiple contents...\n";

    client::ReadResourceResult result;

    // Text content
    client::TextResourceContent text;
    text.uri = "file:///doc.txt";
    text.text = "Hello";
    result.contents.push_back(text);

    // Blob content
    client::BlobResourceContent blob;
    blob.uri = "file:///img.png";
    blob.blob = "iVBORw0KGgo="; // Partial PNG header base64
    blob.mimeType = "image/png";
    result.contents.push_back(blob);

    assert(result.contents.size() == 2);

    // Check first is text
    assert(std::holds_alternative<client::TextResourceContent>(result.contents[0]));
    auto& c1 = std::get<client::TextResourceContent>(result.contents[0]);
    assert(c1.text == "Hello");

    // Check second is blob
    assert(std::holds_alternative<client::BlobResourceContent>(result.contents[1]));
    auto& c2 = std::get<client::BlobResourceContent>(result.contents[1]);
    assert(c2.mimeType.value() == "image/png");

    std::cout << "  [PASS] ReadResourceResult with mixed contents works\n";
}

void test_resource_uri_patterns()
{
    std::cout << "Test 21: Various URI patterns...\n";

    std::vector<std::string> valid_uris = {
        "file:///path/to/file.txt",      "mem://resource-name",
        "http://example.com/resource",   "https://api.example.com/v1/data",
        "custom://my-protocol/resource", "db://postgres/users/123",
        "s3://bucket/key/path"};

    for (const auto& uri : valid_uris)
    {
        client::ResourceInfo info;
        info.uri = uri;
        info.name = "Test";

        Json j;
        to_json(j, info);

        client::ResourceInfo parsed;
        from_json(j, parsed);

        assert(parsed.uri == uri);
    }

    std::cout << "  [PASS] Various URI patterns handled correctly\n";
}

void test_resource_with_complex_annotations()
{
    std::cout << "Test 22: ResourceInfo with complex annotations...\n";

    client::ResourceInfo info;
    info.uri = "file:///data.json";
    info.name = "Data File";
    info.annotations = Json{
        {"tags", Json::array({"important", "reviewed", "v2"})},
        {"metadata", Json{{"created", "2025-01-01"}, {"modified", "2025-01-15"}, {"size", 4096}}},
        {"permissions", Json{{"read", true}, {"write", false}}}};

    Json j;
    to_json(j, info);

    client::ResourceInfo parsed;
    from_json(j, parsed);

    assert(parsed.annotations.has_value());
    auto& ann = parsed.annotations.value();
    assert(ann["tags"].size() == 3);
    assert(ann["metadata"]["size"] == 4096);
    assert(ann["permissions"]["read"] == true);

    std::cout << "  [PASS] Complex annotations preserved correctly\n";
}

void test_embedded_resource_content()
{
    std::cout << "Test 23: EmbeddedResourceContent parsing...\n";

    // Text embedded resource
    Json text_json = {
        {"type", "resource"}, {"uri", "mem://embedded-doc"}, {"text", "Embedded text content"}};

    auto text_block = client::parse_content_block(text_json);
    assert(std::holds_alternative<client::EmbeddedResourceContent>(text_block));
    auto& text_res = std::get<client::EmbeddedResourceContent>(text_block);
    assert(text_res.uri == "mem://embedded-doc");
    assert(text_res.text == "Embedded text content");

    // Blob embedded resource
    Json blob_json = {{"type", "resource"},
                      {"uri", "file:///embedded.bin"},
                      {"blob", "AAEC"},
                      {"mimeType", "application/octet-stream"}};

    auto blob_block = client::parse_content_block(blob_json);
    assert(std::holds_alternative<client::EmbeddedResourceContent>(blob_block));
    auto& blob_res = std::get<client::EmbeddedResourceContent>(blob_block);
    assert(blob_res.uri == "file:///embedded.bin");
    assert(blob_res.blob.value() == "AAEC");
    assert(blob_res.mimeType.value() == "application/octet-stream");

    std::cout << "  [PASS] EmbeddedResourceContent parsed correctly\n";
}

void test_resource_content_without_mimetype()
{
    std::cout << "Test 24: Resource content without mimeType...\n";

    // Text without mimeType
    Json text_json = {{"uri", "mem://plain"}, {"text", "Plain text"}};

    client::TextResourceContent text;
    from_json(text_json, text);
    assert(!text.mimeType.has_value());
    assert(text.text == "Plain text");

    // Blob without mimeType
    Json blob_json = {{"uri", "mem://binary"}, {"blob", "data"}};

    client::BlobResourceContent blob;
    from_json(blob_json, blob);
    assert(!blob.mimeType.has_value());

    std::cout << "  [PASS] Content without mimeType handled correctly\n";
}

void test_resource_pagination()
{
    std::cout << "Test 25: Resource pagination fields...\n";

    // With cursor
    client::ListResourcesResult with_cursor;
    with_cursor.nextCursor = "page_2_token";
    assert(with_cursor.nextCursor.has_value());
    assert(with_cursor.nextCursor.value() == "page_2_token");

    // Without cursor (last page)
    client::ListResourcesResult last_page;
    assert(!last_page.nextCursor.has_value());

    // With metadata
    client::ListResourcesResult with_meta;
    with_meta._meta = Json{{"total", 100}, {"page", 1}};
    assert(with_meta._meta.has_value());
    assert(with_meta._meta.value()["total"] == 100);

    std::cout << "  [PASS] Pagination fields work correctly\n";
}

int main()
{
    std::cout << "Running advanced resources tests...\n\n";

    try
    {
        // Server-side ResourceManager tests (1-10)
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

        // Client-side resource type tests (11-25)
        test_resource_info_serialization();
        test_resource_info_minimal();
        test_resource_template_fields();
        test_text_resource_content_parsing();
        test_blob_resource_content_parsing();
        test_parse_resource_content_text();
        test_parse_resource_content_blob();
        test_list_resources_result();
        test_list_resource_templates_result();
        test_read_resource_result();
        test_resource_uri_patterns();
        test_resource_with_complex_annotations();
        test_embedded_resource_content();
        test_resource_content_without_mimetype();
        test_resource_pagination();

        std::cout << "\n[OK] All 25 advanced resources tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n[FAIL] Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
