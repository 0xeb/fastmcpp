#include <cassert>
#include "fastmcpp/util/schema_build.hpp"

int main() {
  using namespace fastmcpp;
  Json simple{{"name","string"},{"age","integer"},{"active","boolean"}};
  auto schema = util::schema_build::to_object_schema_from_simple(simple);
  assert(schema.at("type") == "object");
  assert(schema.at("properties").at("name").at("type") == "string");
  assert(schema.at("properties").at("age").at("type") == "integer");
  assert(schema.at("properties").at("active").at("type") == "boolean");
  // required includes all keys
  auto req = schema.at("required");
  assert(std::find(req.begin(), req.end(), "name") != req.end());
  assert(std::find(req.begin(), req.end(), "age") != req.end());
  assert(std::find(req.begin(), req.end(), "active") != req.end());

  // Already-a-schema path returns unchanged
  Json already{{"type","object"},{"properties", Json::object({{"x", Json{{"type","number"}}}})}};
  auto schema2 = util::schema_build::to_object_schema_from_simple(already);
  assert(schema2 == already);
  return 0;
}

