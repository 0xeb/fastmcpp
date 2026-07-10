// Resource template metadata propagation tests
// F2 — Python fastmcp #4061 (commit 01b971d8): when instantiating a Resource
// from a ResourceTemplate, the template-level title/annotations/icons/version
// must propagate to the resulting Resource instead of being dropped.

#include "fastmcpp/resources/template.hpp"

#include "fastmcpp/resources/resource.hpp"
#include "fastmcpp/types.hpp"

#include <cassert>
#include <iostream>
#include <unordered_map>

using namespace fastmcpp::resources;
using namespace fastmcpp;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main()
{
    ResourceTemplate templ;
    templ.uri_template = "data://{kind}/{id}";
    templ.name = "data_template";
    templ.description = "A data resource template";
    templ.mime_type = "application/json";
    templ.version = "1.2.3";

    templ.title = std::string("My Data Template");

    Json ann = Json::object();
    ann["audience"] = Json::array({"user"});
    ann["priority"] = 0.9;
    ann["lastModified"] = "2026-06-09T00:00:00Z";
    templ.annotations = ann;

    std::vector<Icon> icons;
    Icon icon;
    icon.src = "https://example.com/icon.png";
    icon.mime_type = std::string("image/png");
    icons.push_back(icon);
    templ.icons = icons;

    templ.task_support = TaskSupport::Optional;
    templ.parameters = Json::object();
    templ.provider = [](const Json&)
    {
        ResourceContent content;
        content.uri = "data://x/1";
        content.data = std::string("{\"ok\":true}");
        return content;
    };
    templ.parse();

    std::unordered_map<std::string, std::string> params = {{"kind", "user"}, {"id", "42"}};
    Resource resource = templ.create_resource("data://user/42", params);

    // Pre-existing fields still propagate
    ASSERT_TRUE(resource.uri == "data://user/42", "uri propagated");
    ASSERT_TRUE(resource.name == "data_template", "name propagated");
    ASSERT_TRUE(resource.description.has_value(), "description present");
    ASSERT_TRUE(*resource.description == "A data resource template", "description propagated");
    ASSERT_TRUE(resource.mime_type.has_value() && *resource.mime_type == "application/json",
                "mime_type propagated");

    // F2: new fields must now propagate
    ASSERT_TRUE(resource.title.has_value(), "title present (F2)");
    ASSERT_TRUE(*resource.title == "My Data Template", "title value preserved (F2)");

    ASSERT_TRUE(resource.annotations.has_value(), "annotations present (F2)");
    ASSERT_TRUE(resource.annotations->contains("audience"), "annotations.audience preserved (F2)");
    ASSERT_TRUE(resource.annotations->at("priority") == 0.9, "annotations.priority preserved (F2)");

    ASSERT_TRUE(resource.icons.has_value(), "icons present (F2)");
    ASSERT_TRUE(resource.icons->size() == 1, "icons count (F2)");
    ASSERT_TRUE(resource.icons->at(0).src == "https://example.com/icon.png",
                "icon src preserved (F2)");

    ASSERT_TRUE(resource.version.has_value(), "version present (F2)");
    ASSERT_TRUE(*resource.version == "1.2.3", "version preserved (F2)");

    ASSERT_TRUE(resource.task_support == TaskSupport::Optional, "task_support preserved (F2)");

    // Provider still callable
    Json extra = Json::object();
    auto content = resource.provider(extra);
    ASSERT_TRUE(content.uri == "data://x/1", "provider still callable");

    std::cout << "PASS: template metadata propagation (F2)" << std::endl;
    return 0;
}
