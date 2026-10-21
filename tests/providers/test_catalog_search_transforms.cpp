#include "fastmcpp/providers/local_provider.hpp"
#include "fastmcpp/providers/transforms/catalog.hpp"
#include "fastmcpp/providers/transforms/search/bm25.hpp"
#include "fastmcpp/providers/transforms/search/regex.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace fastmcpp;

namespace
{

tools::Tool make_tool(const std::string& name, const std::string& description)
{
    Json schema = {
        {"type", "object"},
        {"properties", {{"x", {{"type", "integer"}}}}},
    };
    tools::Tool::Fn fn = [](const Json& args) -> Json { return args; };
    tools::Tool t(name, std::move(schema), Json::object(), std::move(fn));
    t.set_description(description);
    return t;
}

// ---- CatalogTransform tests ----

void test_catalog_transform_passthrough()
{
    std::cout << "test_catalog_transform_passthrough..." << std::flush;

    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_tool("tool_a", "Alpha tool"));
    provider->add_tool(make_tool("tool_b", "Beta tool"));

    // Default CatalogTransform should pass through unchanged
    provider->add_transform(std::make_shared<providers::transforms::CatalogTransform>());

    auto tools = provider->list_tools_transformed();
    assert(tools.size() == 2);
    std::cout << " OK\n";
}

void test_catalog_transform_bypass()
{
    std::cout << "test_catalog_transform_bypass..." << std::flush;

    // A custom CatalogTransform subclass that replaces tools
    struct ReplacingTransform : providers::transforms::CatalogTransform
    {
        std::vector<tools::Tool>
        transform_tools(const providers::transforms::ListToolsNext& call_next) const override
        {
            // Replace with a synthetic tool, but verify bypass works
            auto real = get_tool_catalog(call_next);
            assert(real.size() == 2); // should see real tools, not our replacement

            Json schema = {{"type", "object"}};
            tools::Tool::Fn fn = [](const Json&) -> Json { return "synthetic"; };
            return {tools::Tool("synthetic", std::move(schema), Json::object(), std::move(fn))};
        }
    };

    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_tool("tool_a", "Alpha tool"));
    provider->add_tool(make_tool("tool_b", "Beta tool"));
    provider->add_transform(std::make_shared<ReplacingTransform>());

    auto tools = provider->list_tools_transformed();
    assert(tools.size() == 1);
    assert(tools[0].name() == "synthetic");
    std::cout << " OK\n";
}

// ---- RegexSearchTransform tests ----

void test_regex_search_basic()
{
    std::cout << "test_regex_search_basic..." << std::flush;

    using namespace providers::transforms::search;

    RegexSearchTransform::Options opts;
    opts.max_results = 5;
    RegexSearchTransform transform(opts);

    auto tool_a = make_tool("add_numbers", "Add two numbers together");
    auto tool_b = make_tool("multiply_values", "Multiply two values");
    auto tool_c = make_tool("subtract_numbers", "Subtract one number from another");

    std::vector<tools::Tool> catalog = {tool_a, tool_b, tool_c};

    // Search for "number" should match add_numbers and subtract_numbers
    auto results = transform.do_search(catalog, "number");
    assert(results.size() == 2);
    assert(results[0].name() == "add_numbers");
    assert(results[1].name() == "subtract_numbers");

    std::cout << " OK\n";
}

void test_regex_search_max_results()
{
    std::cout << "test_regex_search_max_results..." << std::flush;

    using namespace providers::transforms::search;

    RegexSearchTransform::Options opts;
    opts.max_results = 1;
    RegexSearchTransform transform(opts);

    auto tool_a = make_tool("add_numbers", "Add two numbers");
    auto tool_b = make_tool("subtract_numbers", "Subtract numbers");

    std::vector<tools::Tool> catalog = {tool_a, tool_b};

    auto results = transform.do_search(catalog, "number");
    assert(results.size() == 1);

    std::cout << " OK\n";
}

void test_regex_search_invalid_pattern()
{
    std::cout << "test_regex_search_invalid_pattern..." << std::flush;

    using namespace providers::transforms::search;

    RegexSearchTransform transform;
    auto tool_a = make_tool("add", "Add");
    std::vector<tools::Tool> catalog = {tool_a};

    auto results = transform.do_search(catalog, "[invalid");
    assert(results.empty());

    std::cout << " OK\n";
}

// ---- Bounded regex search (fastmcp eddc7800) ----
// PYTHON REFERENCE: tests/server/transforms/test_search_regex.py

tools::Tool make_bare_tool(const std::string& name, const std::string& description)
{
    tools::Tool::Fn fn = [](const Json& args) -> Json { return args; };
    tools::Tool t(name, Json{{"type", "object"}, {"properties", Json::object()}}, Json::object(),
                  std::move(fn));
    t.set_description(description);
    return t;
}

std::vector<std::string> names_of(const std::vector<tools::Tool>& tools)
{
    std::vector<std::string> names;
    for (const auto& tool : tools)
        names.push_back(tool.name());
    return names;
}

void test_regex_search_overlong_pattern()
{
    std::cout << "test_regex_search_overlong_pattern..." << std::flush;
    using namespace providers::transforms::search;
    RegexSearchTransform transform;
    std::vector<tools::Tool> catalog = {make_bare_tool("send_email", "Send an email.")};
    assert(transform.do_search(catalog, "send_email|" + std::string(1000, 'x')).empty());
    assert(transform.do_search(catalog, "send_email").size() == 1);
    std::cout << " OK\n";
}

void test_regex_search_syntax_table()
{
    std::cout << "test_regex_search_syntax_table..." << std::flush;
    using namespace providers::transforms::search;
    RegexSearchTransform transform;
    std::vector<tools::Tool> catalog = {
        make_bare_tool("send_email", "Send an email and record the email.")};
    const std::vector<std::string> match = {"send_email"};
    const std::vector<std::string> none = {};
    const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
        {"EMAIL", match},
        {"send.*email|notify", match},
        {R"(\bSEND_[a-z]+\b)", match},
        {"^send_email", match},
        {"(?-i:SEND_EMAIL)", none},
        {"send(?=_email)", none},
        {"(?<=send_)email", none},
        {R"((email).*\1)", none},
        {R"(.*\Z)", none},
        {R"(email\.\z)", match},
        {"(?a)email", none},
        {"[invalid", none},
        {"(?:send)_email", match},
        {"[]_]email", match},
        {"(send|mail)+", match},
        {"(?x) send _ email", match},
        {"(?i)(?-i:send)_EMAIL", match},
    };
    for (const auto& [pattern, expected] : table)
    {
        const auto got = names_of(transform.do_search(catalog, pattern));
        if (got != expected)
        {
            std::cerr << "\n  pattern " << pattern << " gave " << got.size() << " matches\n";
            assert(false);
        }
    }
    std::cout << " OK\n";
}

void test_regex_search_repeated_alternatives_bounded()
{
    std::cout << "test_regex_search_repeated_alternatives_bounded..." << std::flush;
    using namespace providers::transforms::search;
    RegexSearchTransform transform;
    std::vector<tools::Tool> catalog = {make_bare_tool("summarize", std::string(64, 'a') + "!")};
    // Upstream test_search_repeated_alternatives: the engine is linear, so these patterns are
    // accepted and answer quickly (no match because the text ends with '!').
    const auto started = std::chrono::steady_clock::now();
    for (const char* costly : {"(a|aa)+$", "(a+)+$", "^(a|a)*b"})
        assert(transform.do_search(catalog, costly).empty());
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    const std::vector<std::string> hit = {"summarize"};
    assert(names_of(transform.do_search(catalog, "a+!$")) == hit);
    assert(names_of(transform.do_search(catalog, "(a|aa)+!$")) == hit);
    assert(names_of(transform.do_search(catalog, "(a|b)+")) == hit);
    assert(names_of(transform.do_search(catalog, "(?:aa)+!")) == hit);
    assert(names_of(transform.do_search(catalog, "^sum.*?ize")) == hit);
    std::cout << " OK\n";
}

void test_regex_search_time_budget()
{
    std::cout << "test_regex_search_time_budget..." << std::flush;
    using namespace providers::transforms::search;
    using clock = std::chrono::steady_clock;
    RegexSearchTransform transform;
    std::string lorem;
    for (int i = 0; i < 170; ++i)
        lorem += "lorem ";
    std::vector<tools::Tool> catalog;
    for (int i = 0; i < 50; ++i)
        catalog.push_back(make_bare_tool("tool_" + std::to_string(i), lorem));

    // Upstream test_costly_pattern_stops_within_the_search_budget: counted repetition keeps the
    // engine linear but slow, so a 50-tool catalog must hit the 1 s budget and stop under 3 s.
    const auto started = clock::now();
    assert(transform.do_search(catalog, "(?:[^z]?){9000}zzz").empty());
    const auto elapsed = clock::now() - started;
    std::cout << " (" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " ms)";
    assert(elapsed < std::chrono::seconds(3));
    std::cout << " OK\n";
}

void test_regex_transform_list_tools()
{
    std::cout << "test_regex_transform_list_tools..." << std::flush;

    using namespace providers::transforms::search;

    auto provider = std::make_shared<providers::LocalProvider>();
    provider->add_tool(make_tool("tool_a", "Alpha"));
    provider->add_tool(make_tool("tool_b", "Beta"));

    RegexSearchTransform::Options opts;
    opts.always_visible = {"tool_a"};
    provider->add_transform(std::make_shared<RegexSearchTransform>(opts));

    auto tools = provider->list_tools_transformed();
    // Should have: tool_a (pinned) + search_tools + call_tool = 3
    assert(tools.size() == 3);

    bool has_pinned = false, has_search = false, has_call = false;
    for (const auto& t : tools)
        if (t.name() == "tool_a")
            has_pinned = true;
        else if (t.name() == "search_tools")
            has_search = true;
        else if (t.name() == "call_tool")
            has_call = true;
    assert(has_pinned);
    assert(has_search);
    assert(has_call);

    std::cout << " OK\n";
}

// ---- BM25SearchTransform tests ----

void test_bm25_search_basic()
{
    std::cout << "test_bm25_search_basic..." << std::flush;

    using namespace providers::transforms::search;

    BM25SearchTransform transform;

    auto tool_a = make_tool("file_reader", "Read files from the filesystem");
    auto tool_b = make_tool("web_fetcher", "Fetch web pages from URLs");
    auto tool_c = make_tool("db_query", "Query database tables with SQL");

    std::vector<tools::Tool> catalog = {tool_a, tool_b, tool_c};

    auto results = transform.do_search(catalog, "read file");
    assert(!results.empty());
    assert(results[0].name() == "file_reader");

    std::cout << " OK\n";
}

void test_bm25_search_relevance()
{
    std::cout << "test_bm25_search_relevance..." << std::flush;

    using namespace providers::transforms::search;

    BM25SearchTransform::Options opts;
    opts.max_results = 2;
    BM25SearchTransform transform(opts);

    auto tool_a = make_tool("calculator", "Perform mathematical calculations");
    auto tool_b = make_tool("text_editor", "Edit text files");
    auto tool_c = make_tool("math_solver", "Solve math equations and problems");

    std::vector<tools::Tool> catalog = {tool_a, tool_b, tool_c};

    auto results = transform.do_search(catalog, "math calculations");
    assert(!results.empty());
    // Both calculator and math_solver should be in results
    bool has_calc = false, has_math = false;
    for (const auto& r : results)
    {
        if (r.name() == "calculator")
            has_calc = true;
        if (r.name() == "math_solver")
            has_math = true;
    }
    assert(has_calc || has_math);

    std::cout << " OK\n";
}

void test_bm25_search_empty_query()
{
    std::cout << "test_bm25_search_empty_query..." << std::flush;

    using namespace providers::transforms::search;
    BM25SearchTransform transform;

    auto tool_a = make_tool("tool", "A tool");
    std::vector<tools::Tool> catalog = {tool_a};

    auto results = transform.do_search(catalog, "");
    assert(results.empty());

    std::cout << " OK\n";
}

void test_extract_searchable_text()
{
    std::cout << "test_extract_searchable_text..." << std::flush;

    using namespace providers::transforms::search;

    auto tool = make_tool("my_tool", "A cool tool");
    auto text = extract_searchable_text(tool);
    assert(text.find("my_tool") != std::string::npos);
    assert(text.find("A cool tool") != std::string::npos);
    assert(text.find("x") != std::string::npos); // parameter name

    std::cout << " OK\n";
}

} // namespace

int main()
{
    // CatalogTransform tests
    test_catalog_transform_passthrough();
    test_catalog_transform_bypass();

    // RegexSearchTransform tests
    test_regex_search_basic();
    test_regex_search_max_results();
    test_regex_search_invalid_pattern();
    test_regex_transform_list_tools();
    test_regex_search_overlong_pattern();
    test_regex_search_syntax_table();
    test_regex_search_repeated_alternatives_bounded();
    test_regex_search_time_budget();

    // BM25SearchTransform tests
    test_bm25_search_basic();
    test_bm25_search_relevance();
    test_bm25_search_empty_query();

    // Utility tests
    test_extract_searchable_text();

    std::cout << "\nAll catalog/search transform tests passed!\n";
    return 0;
}
