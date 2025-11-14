#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace fastmcpp::util::json {

using json = nlohmann::json;

inline json parse(const std::string& s) { return json::parse(s); }
inline std::string dump(const json& j) { return j.dump(); }
inline std::string dump_pretty(const json& j, int indent = 2) { return j.dump(indent); }

// Convenience helpers
template <typename T>
inline json to_json(const T& value) { return value; }

} // namespace fastmcpp::util::json
