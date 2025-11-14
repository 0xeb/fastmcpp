#include <cassert>
#include "fastmcpp/util/json_schema.hpp"

int main() {
  using namespace fastmcpp;
  Json schema = {
    {"type","object"},
    {"required", Json::array({"a","b"})},
    {"properties", {
      {"a", Json{{"type","integer"}}},
      {"b", Json{{"type","integer"}}}
    }}
  };
  Json good{{"a",2},{"b",3}};
  util::schema::validate(schema, good);

  bool failed = false;
  try { util::schema::validate(schema, Json{{"a","x"}}); } catch (const ValidationError&) { failed = true; }
  assert(failed);
  return 0;
}

