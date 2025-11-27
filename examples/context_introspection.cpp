// Example demonstrating Context API for resource and prompt introspection (v2.13.0+)
//
// This example shows how tools can use the Context class to discover and access
// available resources and prompts at runtime. This mirrors the Python fastmcp
// Context API for introspection capabilities.

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"
#include "fastmcpp/tools/manager.hpp"
#include "fastmcpp/tools/tool.hpp"

#include <iomanip>
#include <iostream>

int main()
{
    using namespace fastmcpp;
    using Json = nlohmann::json;

    std::cout << "=== Context Introspection Example (v2.13.0+) ===\n\n";

    // ============================================================================
    // Step 1: Set up Resources
    // ============================================================================

    resources::ResourceManager resource_mgr;

    // Register some sample resources
    resources::Resource doc1{Id{"file://docs/readme.txt"}, resources::Kind::File,
                             Json{{"description", "Project README"}, {"size", 1024}}};
    resource_mgr.register_resource(doc1);

    resources::Resource doc2{Id{"file://docs/api.txt"}, resources::Kind::File,
                             Json{{"description", "API Documentation"}, {"size", 2048}}};
    resource_mgr.register_resource(doc2);

    resources::Resource config{Id{"config://app.json"}, resources::Kind::Json,
                               Json{{"description", "Application config"}}};
    resource_mgr.register_resource(config);

    // ============================================================================
    // Step 2: Set up Prompts
    // ============================================================================

    prompts::PromptManager prompt_mgr;

    // Register some sample prompts with template variables
    prompts::Prompt greeting("Hello {{name}}, welcome to {{app}}!");
    prompt_mgr.add("greeting", greeting);

    prompts::Prompt summary("Summarize {{topic}} in {{length}} words.");
    prompt_mgr.add("summary_prompt", summary);

    // ============================================================================
    // Step 3: Create Context and demonstrate introspection
    // ============================================================================

    server::Context ctx(resource_mgr, prompt_mgr);

    std::cout << "1. Listing Resources:\n";
    std::cout << "   " << std::string(40, '-') << "\n";
    auto resources = ctx.list_resources();
    for (const auto& res : resources)
    {
        std::cout << "   - URI: " << res.id.value << "\n";
        std::cout << "     Kind: " << resources::to_string(res.kind) << "\n";
        std::cout << "     Metadata: " << res.metadata.dump() << "\n\n";
    }

    std::cout << "\n2. Listing Prompts:\n";
    std::cout << "   " << std::string(40, '-') << "\n";
    auto prompts = ctx.list_prompts();
    for (const auto& [name, prompt] : prompts)
    {
        std::cout << "   - Name: " << name << "\n";
        std::cout << "     Template: " << prompt.template_string() << "\n\n";
    }

    std::cout << "\n3. Getting and Rendering Prompts:\n";
    std::cout << "   " << std::string(40, '-') << "\n";
    try
    {
        Json args = {{"name", "Alice"}, {"app", "FastMCP"}};
        std::string rendered = ctx.get_prompt("greeting", args);
        std::cout << "   Rendered greeting: " << rendered << "\n\n";

        args = {{"topic", "machine learning"}, {"length", "50"}};
        rendered = ctx.get_prompt("summary_prompt", args);
        std::cout << "   Rendered summary: " << rendered << "\n\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "   Error: " << e.what() << "\n\n";
    }

    std::cout << "\n4. Reading Resources:\n";
    std::cout << "   " << std::string(40, '-') << "\n";
    try
    {
        std::string content = ctx.read_resource("file://docs/readme.txt");
        std::cout << "   " << content << "\n\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "   Error: " << e.what() << "\n\n";
    }

    // ============================================================================
    // Step 4: Demonstrate Context usage in a Tool
    // ============================================================================

    std::cout << "\n5. Using Context in a Tool:\n";
    std::cout << "   " << std::string(40, '-') << "\n";

    tools::ToolManager tool_mgr;

    // Define a tool that uses Context for introspection
    tools::Tool analyze_resources{
        "analyze_resources",
        Json{{"type", "object"},
             {"properties",
              Json{{"filter_kind",
                    Json{{"type", "string"}, {"enum", Json::array({"file", "json", "text"})}}}}}},
        Json{{"type", "object"}}, [&resource_mgr, &prompt_mgr](const Json& input) -> Json
        {
            // Create context for introspection
            server::Context ctx(resource_mgr, prompt_mgr);

            // List all resources
            auto all_resources = ctx.list_resources();

            // Filter by kind if specified
            std::string filter = input.value("filter_kind", std::string(""));
            int count = 0;
            Json results = Json::array();

            for (const auto& res : all_resources)
            {
                std::string kind_str = resources::to_string(res.kind);
                if (filter.empty() || kind_str == filter)
                {
                    results.push_back(Json{
                        {"uri", res.id.value}, {"kind", kind_str}, {"metadata", res.metadata}});
                    count++;
                }
            }

            return Json{
                {"content", Json::array({Json{
                                             {"type", "text"},
                                             {"text", std::string("Found ") +
                                                          std::to_string(count) + " resources"},
                                         },
                                         Json{{"type", "text"}, {"text", results.dump(2)}}})}};
        }};

    tool_mgr.register_tool(analyze_resources);

    // Invoke the tool
    Json tool_input = {{"filter_kind", "file"}};
    std::cout << "   Invoking tool with input: " << tool_input.dump() << "\n";

    try
    {
        Json result = tool_mgr.invoke("analyze_resources", tool_input);
        std::cout << "   Tool result:\n";
        if (result.contains("content") && result["content"].is_array())
        {
            for (const auto& item : result["content"])
                if (item.contains("text"))
                    std::cout << "     " << item["text"].get<std::string>() << "\n";
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "   Error: " << e.what() << "\n";
    }

    std::cout << "\n=== Example Complete ===\n";
    return 0;
}
