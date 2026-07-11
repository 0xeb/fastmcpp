// Discriminator property stays required after the discriminator hint is stripped.
// P7 — Python fastmcp #4297 (9b1a1a04) + #4362 (5fa4f32c): when `_strip_discriminator`
// removes a `discriminator` from an anyOf/oneOf, it must add the discriminator's
// `propertyName` to each variant's `required` so strict validators still enforce it.
//
// fastmcpp strips the discriminator in `strip_discriminator` (src/util/json_schema.cpp);
// this cycle adds the required-preservation. Test: after dereference, the hint is gone
// and every variant lists the discriminator field in `required`.

#include "fastmcpp/util/json_schema.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace fastmcpp;

static bool has_required(const Json& variant, const std::string& name)
{
    if (!variant.contains("required") || !variant["required"].is_array())
        return false;
    for (const auto& r : variant["required"])
        if (r.is_string() && r.get<std::string>() == name)
            return true;
    return false;
}

int main()
{
    Json schema = {
        {"oneOf", Json::array({
                      Json{{"type", "object"},
                           {"properties", Json{{"kind", Json{{"const", "cat"}}},
                                               {"meow", Json{{"type", "boolean"}}}}}},
                      Json{{"type", "object"},
                           {"properties", Json{{"kind", Json{{"const", "dog"}}},
                                               {"bark", Json{{"type", "boolean"}}}}}},
                  })},
        {"discriminator", Json{{"propertyName", "kind"}}},
    };

    Json deref = util::schema::dereference_refs(schema);

    // Discriminator hint removed at the union boundary.
    if (deref.contains("discriminator"))
    {
        std::cerr << "FAIL: discriminator not stripped: " << deref.dump() << std::endl;
        return 1;
    }
    if (!deref.contains("oneOf") || !deref["oneOf"].is_array() || deref["oneOf"].size() != 2)
    {
        std::cerr << "FAIL: oneOf variants lost: " << deref.dump() << std::endl;
        return 1;
    }
    // Each variant now requires the discriminator property.
    for (const auto& variant : deref["oneOf"])
    {
        if (!has_required(variant, "kind"))
        {
            std::cerr << "FAIL: variant missing required 'kind': " << variant.dump() << std::endl;
            return 1;
        }
    }

    std::cout << "PASS: discriminator property preserved as required (P7)" << std::endl;
    return 0;
}
