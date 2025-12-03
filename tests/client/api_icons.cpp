/// @file tests/client/api_icons.cpp
/// @brief Integration tests for title and icons fields on client types

#include "test_helpers.hpp"

void test_tool_with_icons()
{
    std::cout << "Test: tool with title and icons...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();

    // Find the icon_tool
    auto it = std::find_if(tools.begin(), tools.end(),
                           [](const auto& t) { return t.name == "icon_tool"; });

    assert(it != tools.end());
    assert(it->title.has_value());
    assert(*it->title == "My Icon Tool");
    assert(it->icons.has_value());
    assert(it->icons->size() == 2);

    // Check first icon (URL)
    assert((*it->icons)[0].src == "https://example.com/icon.png");
    assert((*it->icons)[0].mime_type.has_value());
    assert(*(*it->icons)[0].mime_type == "image/png");
    assert(!(*it->icons)[0].sizes.has_value());

    // Check second icon (data URI with sizes)
    assert((*it->icons)[1].src == "data:image/svg+xml;base64,PHN2Zz48L3N2Zz4=");
    assert((*it->icons)[1].mime_type.has_value());
    assert(*(*it->icons)[1].mime_type == "image/svg+xml");
    assert((*it->icons)[1].sizes.has_value());
    assert((*it->icons)[1].sizes->size() == 2);
    assert((*(*it->icons)[1].sizes)[0] == "48x48");
    assert((*(*it->icons)[1].sizes)[1] == "any");

    std::cout << "  [PASS] Tool with title and icons round-trips correctly\n";
}

void test_tool_without_icons()
{
    std::cout << "Test: tool without icons...\n";

    auto srv = create_tool_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto tools = c.list_tools();

    // Find the add tool (has no icons)
    auto it =
        std::find_if(tools.begin(), tools.end(), [](const auto& t) { return t.name == "add"; });

    assert(it != tools.end());
    assert(!it->title.has_value());
    assert(!it->icons.has_value());

    std::cout << "  [PASS] Tool without icons has nullopt fields\n";
}

void test_resource_with_icons()
{
    std::cout << "Test: resource with title and icons...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto resources = c.list_resources();

    // Find the icon_resource
    auto it = std::find_if(resources.begin(), resources.end(),
                           [](const auto& r) { return r.name == "icon_resource"; });

    assert(it != resources.end());
    assert(it->title.has_value());
    assert(*it->title == "Resource With Icons");
    assert(it->icons.has_value());
    assert(it->icons->size() == 1);
    assert((*it->icons)[0].src == "https://example.com/res.png");

    std::cout << "  [PASS] Resource with title and icons round-trips correctly\n";
}

void test_resource_template_with_icons()
{
    std::cout << "Test: resource template with title and icons...\n";

    auto srv = create_resource_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto templates = c.list_resource_templates();

    // Find the icon_template
    auto it = std::find_if(templates.begin(), templates.end(),
                           [](const auto& t) { return t.name == "icon_template"; });

    assert(it != templates.end());
    assert(it->title.has_value());
    assert(*it->title == "Template With Icons");
    assert(it->icons.has_value());
    assert(it->icons->size() == 1);
    assert((*it->icons)[0].src == "https://example.com/tpl.svg");
    assert((*it->icons)[0].mime_type.has_value());
    assert(*(*it->icons)[0].mime_type == "image/svg+xml");

    std::cout << "  [PASS] Resource template with title and icons round-trips correctly\n";
}

void test_prompt_with_icons()
{
    std::cout << "Test: prompt with title and icons...\n";

    auto srv = create_prompt_server();
    client::Client c(std::make_unique<client::LoopbackTransport>(srv));

    auto prompts = c.list_prompts();

    // Find the icon_prompt
    auto it = std::find_if(prompts.begin(), prompts.end(),
                           [](const auto& p) { return p.name == "icon_prompt"; });

    assert(it != prompts.end());
    assert(it->title.has_value());
    assert(*it->title == "Prompt With Icons");
    assert(it->icons.has_value());
    assert(it->icons->size() == 1);
    assert((*it->icons)[0].src == "https://example.com/prompt.png");

    std::cout << "  [PASS] Prompt with title and icons round-trips correctly\n";
}

int main()
{
    std::cout << "\n=== Client API Icons Integration Tests ===\n\n";

    test_tool_with_icons();
    test_tool_without_icons();
    test_resource_with_icons();
    test_resource_template_with_icons();
    test_prompt_with_icons();

    std::cout << "\n=== All Icon Integration Tests Passed ===\n\n";
    return 0;
}
