// Tests for CatalogTransform.get_tool_catalog() dedup-by-version + meta
// injection, plus VersionFilter -> CatalogTransform ordering. Parity with
// Python fastmcp commits 03673d9f and 0142fefe (tests/server/transforms/
// test_catalog.py).

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/providers/transforms/catalog.hpp"
#include "fastmcpp/providers/transforms/version_filter.hpp"
#include "fastmcpp/util/versions.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace fastmcpp;
using providers::transforms::CatalogTransform;
using providers::transforms::VersionFilter;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                                       \
    do                                                                                             \
    {                                                                                              \
        if (!((a) == (b)))                                                                         \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

namespace
{
tools::Tool make_tool(const std::string& name, const std::string& version)
{
    tools::Tool t(name, Json::object(), Json::object(),
                  [name, version](const Json&) { return Json(name + "@" + version); });
    if (!version.empty())
        t.set_version(version);
    return t;
}

class NoopCatalogTransform : public CatalogTransform
{
};

class PipelineCatalogTransform : public CatalogTransform
{
  public:
    std::vector<tools::Tool>
    transform_tools(const providers::transforms::ListToolsNext& call_next) const override
    {
        return get_tool_catalog(call_next);
    }
};

class StaticToolProvider : public providers::Provider
{
  public:
    explicit StaticToolProvider(std::vector<tools::Tool> tools) : tools_(std::move(tools)) {}

    std::vector<tools::Tool> list_tools() const override
    {
        return tools_;
    }

  private:
    std::vector<tools::Tool> tools_;
};

// Build a list_tools call_next that simply returns a fixed tool list (we're
// exercising the dedup logic at the catalog accessor, not the pipeline).
auto fixed_call_next(const std::vector<tools::Tool>& tools)
{
    return [tools]() { return tools; };
}
} // namespace

static int test_compare_versions()
{
    std::cout << "  test_compare_versions..." << std::endl;
    using util::versions::compare;
    ASSERT_TRUE(compare(std::optional<std::string>("1"), std::optional<std::string>("2")) < 0,
                "1 < 2");
    ASSERT_TRUE(compare(std::optional<std::string>("2"), std::optional<std::string>("10")) < 0,
                "2 < 10 (numeric, not lex)");
    ASSERT_TRUE(compare(std::optional<std::string>("v1.2"), std::optional<std::string>("1.2")) == 0,
                "v-prefix normalised");
    ASSERT_TRUE(compare(std::nullopt, std::optional<std::string>("1.0")) < 0, "None < anything");
    ASSERT_TRUE(compare(std::optional<std::string>("1.0"), std::optional<std::string>("1.0")) == 0,
                "equal");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_dedupe_keeps_highest()
{
    std::cout << "  test_dedupe_keeps_highest..." << std::endl;
    using util::versions::dedupe_with_versions;
    std::vector<tools::Tool> tools{
        make_tool("greet", "1"),
        make_tool("greet", "2"),
        make_tool("greet", "3"),
    };
    auto deduped = dedupe_with_versions(
        tools, [](const tools::Tool& t) { return t.name(); },
        [](const tools::Tool& t) { return t.version(); });
    ASSERT_EQ(deduped.size(), 1u, "deduped to one");
    ASSERT_TRUE(deduped[0].item.version().has_value() && *deduped[0].item.version() == "3",
                "highest version wins");
    ASSERT_EQ(deduped[0].available_versions.size(), 3u, "three available versions reported");
    ASSERT_TRUE(deduped[0].available_versions[0] == "3", "descending order");
    ASSERT_TRUE(deduped[0].available_versions[1] == "2", "v2 second");
    ASSERT_TRUE(deduped[0].available_versions[2] == "1", "v1 last");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_returns_highest_only()
{
    std::cout << "  test_get_tool_catalog_returns_highest_only..." << std::endl;
    NoopCatalogTransform t;
    auto next = fixed_call_next({
        make_tool("greet", "1"),
        make_tool("greet", "2"),
        make_tool("greet", "3"),
    });
    auto result = t.get_tool_catalog(next);
    ASSERT_EQ(result.size(), 1u, "one tool returned");
    ASSERT_EQ(result[0].name(), std::string("greet"), "greet retained");
    ASSERT_TRUE(result[0].version().has_value() && *result[0].version() == "3", "v3 kept");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_injects_versions_meta()
{
    std::cout << "  test_get_tool_catalog_injects_versions_meta..." << std::endl;
    NoopCatalogTransform t;
    auto next = fixed_call_next({
        make_tool("greet", "1"),
        make_tool("greet", "3"),
    });
    auto result = t.get_tool_catalog(next);
    ASSERT_EQ(result.size(), 1u, "one tool");
    ASSERT_TRUE(result[0].meta().has_value(), "meta present");
    const auto& meta = *result[0].meta();
    ASSERT_TRUE(meta.contains("fastmcp") && meta["fastmcp"].is_object(), "fastmcp block");
    ASSERT_TRUE(meta["fastmcp"].contains("versions") && meta["fastmcp"]["versions"].is_array(),
                "versions array");
    auto versions = meta["fastmcp"]["versions"].get<std::vector<std::string>>();
    ASSERT_EQ(versions.size(), 2u, "two versions");
    ASSERT_TRUE(versions[0] == "3" && versions[1] == "1", "descending order matches Python");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_mixed_versioned_unversioned()
{
    std::cout << "  test_get_tool_catalog_mixed_versioned_unversioned..." << std::endl;
    NoopCatalogTransform t;
    auto next = fixed_call_next({
        make_tool("standalone", ""), // unversioned, distinct key
        make_tool("greet", "1"),
        make_tool("greet", "2"),
    });
    auto result = t.get_tool_catalog(next);
    ASSERT_EQ(result.size(), 2u, "two distinct keys");

    // standalone first (insertion order preserved); greet@2 second.
    ASSERT_EQ(result[0].name(), std::string("standalone"), "standalone first");
    ASSERT_TRUE(!result[0].version().has_value(), "standalone unversioned");
    ASSERT_TRUE(!result[0].meta().has_value() || !result[0].meta()->contains("fastmcp") ||
                    !(*result[0].meta())["fastmcp"].contains("versions"),
                "no versions meta when single-version (or no version) entry");

    ASSERT_EQ(result[1].name(), std::string("greet"), "greet second");
    ASSERT_TRUE(result[1].version().has_value() && *result[1].version() == "2", "greet v2");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_no_meta_for_single_version()
{
    std::cout << "  test_no_meta_for_single_version..." << std::endl;
    NoopCatalogTransform t;
    auto next = fixed_call_next({
        make_tool("solo", "1.5"),
    });
    auto result = t.get_tool_catalog(next);
    ASSERT_EQ(result.size(), 1u, "one tool");
    // Single version still publishes meta.fastmcp.versions=[1.5] per Python
    // (any(c.version is not None) holds true for single versioned entry).
    ASSERT_TRUE(result[0].meta().has_value(), "meta present even for single version");
    auto v = (*result[0].meta())["fastmcp"]["versions"].get<std::vector<std::string>>();
    ASSERT_EQ(v.size(), 1u, "one version listed");
    ASSERT_TRUE(v[0] == "1.5", "version preserved");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_coerces_scalar_meta_to_object()
{
    std::cout << "  test_get_tool_catalog_coerces_scalar_meta_to_object..." << std::endl;
    NoopCatalogTransform t;

    auto v1 = make_tool("greet", "1");
    auto v3 = make_tool("greet", "3");
    v3.set_meta(Json("scalar-meta"));

    auto result = t.get_tool_catalog(fixed_call_next({v1, v3}));
    ASSERT_EQ(result.size(), 1u, "one tool");
    ASSERT_TRUE(result[0].meta().has_value(), "meta present");
    ASSERT_TRUE(result[0].meta()->is_object(), "scalar meta coerced to object");
    ASSERT_TRUE((*result[0].meta()).contains("fastmcp"), "fastmcp block present");
    auto versions = (*result[0].meta())["fastmcp"]["versions"].get<std::vector<std::string>>();
    ASSERT_EQ(versions.size(), 2u, "two versions preserved");
    ASSERT_TRUE(versions[0] == "3" && versions[1] == "1", "version order preserved");

    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_coerces_array_meta_to_object()
{
    std::cout << "  test_get_tool_catalog_coerces_array_meta_to_object..." << std::endl;
    NoopCatalogTransform t;

    auto v1 = make_tool("greet", "1");
    auto v3 = make_tool("greet", "3");
    v3.set_meta(Json::array({"alpha", "beta"}));

    auto result = t.get_tool_catalog(fixed_call_next({v1, v3}));
    ASSERT_EQ(result.size(), 1u, "one tool");
    ASSERT_TRUE(result[0].meta().has_value(), "meta present");
    ASSERT_TRUE(result[0].meta()->is_object(), "array meta coerced to object");
    ASSERT_TRUE((*result[0].meta()).contains("fastmcp"), "fastmcp block present");
    ASSERT_TRUE((*result[0].meta())["fastmcp"]["versions"].is_array(), "versions injected");

    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_get_tool_catalog_preserves_existing_object_meta()
{
    std::cout << "  test_get_tool_catalog_preserves_existing_object_meta..." << std::endl;
    NoopCatalogTransform t;

    auto v1 = make_tool("greet", "1");
    auto v3 = make_tool("greet", "3");
    v3.set_meta(Json{{"custom", "value"}, {"fastmcp", Json{{"source", "canonical"}}}});

    auto result = t.get_tool_catalog(fixed_call_next({v1, v3}));
    ASSERT_EQ(result.size(), 1u, "one tool");
    ASSERT_TRUE(result[0].meta().has_value(), "meta present");
    ASSERT_TRUE(result[0].meta()->is_object(), "object meta retained");
    ASSERT_TRUE((*result[0].meta()).value("custom", std::string{}) == "value", "custom key preserved");
    ASSERT_TRUE((*result[0].meta())["fastmcp"].value("source", std::string{}) == "canonical",
                "existing fastmcp fields preserved");
    auto versions = (*result[0].meta())["fastmcp"]["versions"].get<std::vector<std::string>>();
    ASSERT_EQ(versions.size(), 2u, "versions still injected");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// 0142fefe: VersionFilter applied before CatalogTransform must restrict what
// the catalog accessor sees. We simulate this by chaining a filter call_next.
static int test_version_filter_applied_before_catalog()
{
    std::cout << "  test_version_filter_applied_before_catalog..." << std::endl;
    NoopCatalogTransform t;

    // Simulated filter: only tools with version < "3"
    auto raw = std::vector<tools::Tool>{
        make_tool("greet", "1"),
        make_tool("greet", "2"),
        make_tool("greet", "3"),
    };
    auto filtered_next = [&]()
    {
        std::vector<tools::Tool> filtered;
        for (const auto& tool : raw)
        {
            const auto& v = tool.version();
            if (!v)
                continue;
            // Numeric compare via util::versions::compare (parity with VersionFilter).
            if (util::versions::compare(v, std::optional<std::string>("3")) < 0)
                filtered.push_back(tool);
        }
        return filtered;
    };

    auto result = t.get_tool_catalog(filtered_next);
    ASSERT_EQ(result.size(), 1u, "one tool returned by catalog after filtering");
    ASSERT_TRUE(result[0].version().has_value() && *result[0].version() == "2",
                "highest version under filter is v2");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_metadata_survives_tool_info_and_mcp_serialization()
{
    std::cout << "  test_metadata_survives_tool_info_and_mcp_serialization..." << std::endl;

    auto v1 = make_tool("greet", "1");
    auto v3 = make_tool("greet", "3");
    AppConfig app_config;
    app_config.resource_uri = "ui://widgets/greet.html";
    v3.set_app(app_config);
    auto provider = std::make_shared<StaticToolProvider>(std::vector<tools::Tool>{v1, v3});
    provider->add_transform(std::make_shared<PipelineCatalogTransform>());

    FastMCP app("catalog", "1.0.0");
    app.add_provider(provider);
    auto tools = app.list_all_tools_info();
    ASSERT_EQ(tools.size(), 1u, "deduped tool list");
    ASSERT_TRUE(tools[0]._meta.has_value(), "tool info carries _meta");
    ASSERT_TRUE((*tools[0]._meta).contains("fastmcp"), "fastmcp block present");
    ASSERT_TRUE((*tools[0]._meta)["fastmcp"].contains("versions"), "versions surfaced");
    ASSERT_TRUE((*tools[0]._meta).contains("ui"), "ui metadata preserved");
    auto handler = mcp::make_mcp_handler(app);
    Json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    Json resp = handler(req);
    ASSERT_TRUE(resp.contains("result") && resp["result"].contains("tools"), "tools/list result");
    ASSERT_EQ(resp["result"]["tools"].size(), 1u, "one serialized tool");
    const auto& tool = resp["result"]["tools"][0];
    ASSERT_TRUE(tool.contains("_meta") && tool["_meta"].is_object(), "serialized _meta");
    ASSERT_TRUE(tool["_meta"].contains("fastmcp"), "serialized fastmcp block");
    ASSERT_TRUE(tool["_meta"]["fastmcp"].contains("versions"), "serialized versions");
    ASSERT_TRUE(tool["_meta"]["fastmcp"]["versions"].is_array(), "serialized versions array");
    std::vector<std::string> versions;
    for (const auto& version_json : tool["_meta"]["fastmcp"]["versions"])
    {
        ASSERT_TRUE(version_json.is_string(), "serialized version value is string");
        versions.push_back(version_json.get<std::string>());
    }
    ASSERT_EQ(versions.size(), 2u, "two versions serialized");
    ASSERT_TRUE(versions[0] == "3" && versions[1] == "1", "serialized version order");
    ASSERT_TRUE(tool["_meta"].contains("ui"), "serialized ui preserved");
    ASSERT_TRUE(tool["_meta"]["ui"].is_object(), "serialized ui object");
    ASSERT_TRUE(tool["_meta"]["ui"].value("resourceUri", std::string{}) ==
                    "ui://widgets/greet.html",
                "serialized ui value preserved");

    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_non_object_meta_does_not_break_tool_info_or_mcp_serialization()
{
    std::cout << "  test_non_object_meta_does_not_break_tool_info_or_mcp_serialization..." << std::endl;

    auto v1 = make_tool("greet", "1");
    auto v3 = make_tool("greet", "3");
    v3.set_meta(Json("scalar-meta"));

    auto provider = std::make_shared<StaticToolProvider>(std::vector<tools::Tool>{v1, v3});
    provider->add_transform(std::make_shared<PipelineCatalogTransform>());

    FastMCP app("catalog", "1.0.0");
    app.add_provider(provider);

    auto tools = app.list_all_tools_info();
    ASSERT_EQ(tools.size(), 1u, "deduped tool list");
    ASSERT_TRUE(tools[0]._meta.has_value(), "tool info carries _meta");
    ASSERT_TRUE((*tools[0]._meta).is_object(), "non-object meta coerced before tool info serialization");
    ASSERT_TRUE((*tools[0]._meta).contains("fastmcp"), "fastmcp block present in tool info");
    ASSERT_TRUE((*tools[0]._meta)["fastmcp"].contains("versions"), "versions surfaced in tool info");

    auto handler = mcp::make_mcp_handler(app);
    Json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    Json resp = handler(req);
    ASSERT_TRUE(resp.contains("result") && resp["result"].contains("tools"), "tools/list result");
    ASSERT_EQ(resp["result"]["tools"].size(), 1u, "one serialized tool");
    const auto& tool = resp["result"]["tools"][0];
    ASSERT_TRUE(tool.contains("_meta") && tool["_meta"].is_object(), "serialized _meta");
    ASSERT_TRUE(tool["_meta"].contains("fastmcp"), "serialized fastmcp block");
    ASSERT_TRUE(tool["_meta"]["fastmcp"].contains("versions"), "serialized versions");

    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "CatalogTransform Dedup + Ordering Tests" << std::endl;
    std::cout << "=======================================" << std::endl;
    int failures = 0;
    failures += test_compare_versions();
    failures += test_dedupe_keeps_highest();
    failures += test_get_tool_catalog_returns_highest_only();
    failures += test_get_tool_catalog_injects_versions_meta();
    failures += test_get_tool_catalog_mixed_versioned_unversioned();
    failures += test_no_meta_for_single_version();
    failures += test_get_tool_catalog_coerces_scalar_meta_to_object();
    failures += test_get_tool_catalog_coerces_array_meta_to_object();
    failures += test_get_tool_catalog_preserves_existing_object_meta();
    failures += test_version_filter_applied_before_catalog();
    failures += test_metadata_survives_tool_info_and_mcp_serialization();
    failures += test_non_object_meta_does_not_break_tool_info_or_mcp_serialization();
    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
