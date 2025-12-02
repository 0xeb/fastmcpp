#include "fastmcpp/client/types.hpp"
#include "fastmcpp/types.hpp"
#include "fastmcpp/util/json.hpp"

#include <cassert>
#include <iostream>
#include <string>

using fastmcpp::util::json::dump;
using fastmcpp::util::json::dump_pretty;
using fastmcpp::util::json::json;
using fastmcpp::util::json::parse;

void test_icon_serialization()
{
    // Icon round-trip
    fastmcpp::Icon icon;
    icon.src = "https://example.com/icon.png";
    icon.mime_type = "image/png";
    icon.sizes = std::vector<std::string>{"48x48", "96x96"};

    json j = icon;
    assert(j["src"] == "https://example.com/icon.png");
    assert(j["mimeType"] == "image/png");
    assert(j["sizes"].size() == 2);

    auto icon2 = j.get<fastmcpp::Icon>();
    assert(icon2.src == icon.src);
    assert(icon2.mime_type == icon.mime_type);
    assert(icon2.sizes->size() == 2);
    std::cout << "  Icon serialization: PASS\n";
}

void test_toolinfo_title_icons()
{
    fastmcpp::client::ToolInfo tool;
    tool.name = "my_tool";
    tool.title = "My Tool Title";
    tool.description = "A test tool";
    tool.inputSchema = json{{"type", "object"}};

    fastmcpp::Icon icon;
    icon.src = "https://example.com/tool.png";
    tool.icons = std::vector<fastmcpp::Icon>{icon};

    json j = tool;
    assert(j["name"] == "my_tool");
    assert(j["title"] == "My Tool Title");
    assert(j.contains("icons"));

    auto tool2 = j.get<fastmcpp::client::ToolInfo>();
    assert(tool2.title.has_value());
    assert(*tool2.title == "My Tool Title");
    assert(tool2.icons.has_value());
    std::cout << "  ToolInfo title/icons: PASS\n";
}

void test_resourceinfo_title_icons()
{
    fastmcpp::client::ResourceInfo res;
    res.uri = "file:///test.txt";
    res.name = "test.txt";
    res.title = "Test File";

    fastmcpp::Icon icon;
    icon.src = "data:image/png;base64,abc";
    res.icons = std::vector<fastmcpp::Icon>{icon};

    json j = res;
    assert(j["title"] == "Test File");
    assert(j.contains("icons"));

    auto res2 = j.get<fastmcpp::client::ResourceInfo>();
    assert(res2.title.has_value());
    assert(res2.icons.has_value());
    std::cout << "  ResourceInfo title/icons: PASS\n";
}

void test_resourcetemplate_title_icons()
{
    fastmcpp::client::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{name}";
    tmpl.name = "file_template";
    tmpl.title = "File Template";

    fastmcpp::Icon icon;
    icon.src = "/icons/file.svg";
    tmpl.icons = std::vector<fastmcpp::Icon>{icon};

    json j = tmpl;
    assert(j["title"] == "File Template");
    assert(j.contains("icons"));

    auto tmpl2 = j.get<fastmcpp::client::ResourceTemplate>();
    assert(tmpl2.title.has_value());
    assert(tmpl2.icons.has_value());
    std::cout << "  ResourceTemplate title/icons: PASS\n";
}

void test_promptinfo_title_icons()
{
    fastmcpp::client::PromptInfo prompt;
    prompt.name = "code_review";
    prompt.title = "Code Review Prompt";

    fastmcpp::Icon icon;
    icon.src = "https://example.com/review.png";
    prompt.icons = std::vector<fastmcpp::Icon>{icon};

    json j = prompt;
    assert(j["title"] == "Code Review Prompt");
    assert(j.contains("icons"));

    auto prompt2 = j.get<fastmcpp::client::PromptInfo>();
    assert(prompt2.title.has_value());
    assert(prompt2.icons.has_value());
    std::cout << "  PromptInfo title/icons: PASS\n";
}

void test_types_without_optional_fields()
{
    fastmcpp::client::ToolInfo tool;
    tool.name = "simple";
    tool.inputSchema = json{{"type", "object"}};

    json j = tool;
    assert(!j.contains("title"));
    assert(!j.contains("icons"));

    auto tool2 = j.get<fastmcpp::client::ToolInfo>();
    assert(!tool2.title.has_value());
    assert(!tool2.icons.has_value());
    std::cout << "  Types without optional fields: PASS\n";
}

int main()
{
    std::cout << "Testing JSON type serialization:\n";

    auto j = parse("{\"a\":1,\"b\":[true,\"x\"]}");
    assert(j["a"].get<int>() == 1);
    auto s = dump(j);
    auto sp = dump_pretty(j, 2);
    assert(!s.empty());
    assert(!sp.empty());

    fastmcpp::Id id{"abc"};
    json jid = id;
    auto id2 = jid.get<fastmcpp::Id>();
    assert(id2.value == "abc");
    std::cout << "  Basic JSON operations: PASS\n";

    test_icon_serialization();
    test_toolinfo_title_icons();
    test_resourceinfo_title_icons();
    test_resourcetemplate_title_icons();
    test_promptinfo_title_icons();
    test_types_without_optional_fields();

    std::cout << "All JSON type tests PASSED!\n";
    return 0;
}
