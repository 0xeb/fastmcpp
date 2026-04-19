// Tests for fastmcpp::util::read_fastmcp_metadata().
// Parity with Python fastmcp commit 706b56d5 (Harden fastmcp metadata parsing
// in proxy paths) and reference test in
// reference/fastmcp/tests/utilities/test_components.py.

#include "fastmcpp/types.hpp"
#include "fastmcpp/util/metadata.hpp"

#include <iostream>

using fastmcpp::Json;
using fastmcpp::util::read_fastmcp_metadata;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_returns_object_for_canonical_key()
{
    std::cout << "  test_returns_object_for_canonical_key..." << std::endl;
    Json meta = {{"fastmcp", Json{{"version", "3.1.1"}}}};
    auto out = read_fastmcp_metadata(meta);
    ASSERT_TRUE(out.has_value(), "value present");
    ASSERT_TRUE(out->is_object(), "is object");
    ASSERT_TRUE(out->value("version", std::string{}) == "3.1.1", "version preserved");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_returns_object_for_legacy_key()
{
    std::cout << "  test_returns_object_for_legacy_key..." << std::endl;
    Json meta = {{"_fastmcp", Json{{"versions", Json::array({"1.0", "2.0"})}}}};
    auto out = read_fastmcp_metadata(meta);
    ASSERT_TRUE(out.has_value(), "value present");
    ASSERT_TRUE(out->contains("versions") && (*out)["versions"].is_array(), "versions array");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_canonical_wins_over_legacy()
{
    std::cout << "  test_canonical_wins_over_legacy..." << std::endl;
    Json meta = {
        {"fastmcp", Json{{"source", "canonical"}}},
        {"_fastmcp", Json{{"source", "legacy"}}},
    };
    auto out = read_fastmcp_metadata(meta);
    ASSERT_TRUE(out.has_value(), "value present");
    ASSERT_TRUE(out->value("source", std::string{}) == "canonical", "canonical preferred");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_non_object_scalar_is_dropped()
{
    std::cout << "  test_non_object_scalar_is_dropped..." << std::endl;
    Json meta = {{"fastmcp", "oops"}};
    auto out = read_fastmcp_metadata(meta);
    ASSERT_TRUE(!out.has_value(), "scalar fastmcp dropped");

    meta = {{"fastmcp", 42}};
    ASSERT_TRUE(!read_fastmcp_metadata(meta).has_value(), "int dropped");

    meta = {{"fastmcp", true}};
    ASSERT_TRUE(!read_fastmcp_metadata(meta).has_value(), "bool dropped");

    meta = {{"fastmcp", nullptr}};
    ASSERT_TRUE(!read_fastmcp_metadata(meta).has_value(), "null dropped");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_array_value_is_dropped()
{
    std::cout << "  test_array_value_is_dropped..." << std::endl;
    Json meta = {{"fastmcp", Json::array({"a", "b"})}};
    ASSERT_TRUE(!read_fastmcp_metadata(meta).has_value(), "array dropped");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_falls_back_to_legacy_when_canonical_invalid()
{
    std::cout << "  test_falls_back_to_legacy_when_canonical_invalid..." << std::endl;
    // Per Python's iteration order: canonical key checked first; if value is
    // not a dict, the loop continues and inspects the legacy key.
    Json meta = {
        {"fastmcp", "oops"},
        {"_fastmcp", Json{{"source", "legacy"}}},
    };
    auto out = read_fastmcp_metadata(meta);
    ASSERT_TRUE(out.has_value(), "fell back to legacy");
    ASSERT_TRUE(out->value("source", std::string{}) == "legacy", "legacy returned");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_absent_keys()
{
    std::cout << "  test_absent_keys..." << std::endl;
    Json empty = Json::object();
    ASSERT_TRUE(!read_fastmcp_metadata(empty).has_value(), "empty meta");
    Json other = {{"unrelated", Json::object()}};
    ASSERT_TRUE(!read_fastmcp_metadata(other).has_value(), "no relevant keys");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_non_object_meta()
{
    std::cout << "  test_non_object_meta..." << std::endl;
    Json scalar = "not an object";
    ASSERT_TRUE(!read_fastmcp_metadata(scalar).has_value(), "scalar meta input");
    Json arr = Json::array();
    ASSERT_TRUE(!read_fastmcp_metadata(arr).has_value(), "array meta input");
    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "fastmcpp::util::read_fastmcp_metadata Tests" << std::endl;
    std::cout << "===========================================" << std::endl;

    int failures = 0;
    failures += test_returns_object_for_canonical_key();
    failures += test_returns_object_for_legacy_key();
    failures += test_canonical_wins_over_legacy();
    failures += test_non_object_scalar_is_dropped();
    failures += test_array_value_is_dropped();
    failures += test_falls_back_to_legacy_when_canonical_invalid();
    failures += test_absent_keys();
    failures += test_non_object_meta();

    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
