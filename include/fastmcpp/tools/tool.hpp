#pragma once
#include <string>
#include <functional>
#include "fastmcpp/types.hpp"

namespace fastmcpp::tools {

class Tool {
 public:
  using Fn = std::function<fastmcpp::Json(const fastmcpp::Json&)>;

  Tool() = default;
  Tool(std::string name, fastmcpp::Json input_schema, fastmcpp::Json output_schema, Fn fn)
      : name_(std::move(name)), input_schema_(std::move(input_schema)),
        output_schema_(std::move(output_schema)), fn_(std::move(fn)) {}

  const std::string& name() const { return name_; }
  const fastmcpp::Json& input_schema() const { return input_schema_; }
  const fastmcpp::Json& output_schema() const { return output_schema_; }
  fastmcpp::Json invoke(const fastmcpp::Json& input) const { return fn_(input); }

 private:
  std::string name_;
  fastmcpp::Json input_schema_;
  fastmcpp::Json output_schema_;
  Fn fn_;
};

} // namespace fastmcpp::tools

