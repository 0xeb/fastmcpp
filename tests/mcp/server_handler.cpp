#include "fastmcpp/content.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    server::Server s;
    // generate_chart returns mixed text + image content
    s.route("generate_chart",
            [](const Json& in)
            {
                std::string title = in.value("title", std::string("Untitled"));
                ImageContent img;
                img.data = "BASE64";
                img.mimeType = "image/png";
                Json content = Json::array(
                    {TextContent{"text", std::string("Generated chart: ") + title}, img});
                return Json{{"content", content}};
            });

    std::vector<std::tuple<std::string, std::string, Json>> meta;
    meta.emplace_back("generate_chart", "Generates a chart",
                      Json{{"type", "object"},
                           {"properties", Json::object({{"title", Json{{"type", "string"}}}})},
                           {"required", Json::array({"title"})}});

    auto handler = mcp::make_mcp_handler("viz", "1.0.0", s, meta);

    // list
    Json list = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto list_resp = handler(list);
    assert(list_resp["result"]["tools"].size() == 1);

    // call
    Json call = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", Json{{"name", "generate_chart"}, {"arguments", Json{{"title", "Sales"}}}}}};
    auto call_resp = handler(call);
    auto content = call_resp["result"]["content"];
    assert(content.size() == 2);
    assert(content[0]["type"] == "text");
    assert(content[1]["type"] == "image");
    assert(content[1]["mimeType"] == "image/png");

    // resources/list route
    s.route("resources/list",
            [](const Json&)
            {
                return Json{{"resources", Json::array({Json{{"uri", "file:///readme.txt"},
                                                            {"name", "readme.txt"}}})}};
            });
    Json res_list = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/list"}};
    auto res_resp = handler(res_list);
    assert(res_resp["result"]["resources"].size() == 1);
    assert(res_resp["result"]["resources"][0]["uri"] == "file:///readme.txt");

    // prompts/get route
    s.route("prompts/get",
            [](const Json& in)
            {
                auto args = in.value("arguments", Json::object());
                std::string who = args.value("name", std::string("there"));
                return Json{
                    {"description", "demo"},
                    {"messages", Json::array({Json{{"role", "user"}, {"content", "Hi " + who}}})}};
            });
    Json get_prompt = {{"jsonrpc", "2.0"},
                       {"id", 5},
                       {"method", "prompts/get"},
                       {"params", Json{{"name", "prompt1"}, {"arguments", Json{{"name", "Bob"}}}}}};
    auto prompt_resp = handler(get_prompt);
    assert(prompt_resp["result"]["messages"].size() == 1);
    assert(prompt_resp["result"]["messages"][0]["role"] == "user");
    assert(prompt_resp["result"]["messages"][0]["content"] == "Hi Bob");
    return 0;
}
