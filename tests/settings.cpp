#include <cassert>
#include <cstdlib>
#include "fastmcpp/settings.hpp"

// Cross-platform setenv wrapper
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

int main() {
  using namespace fastmcpp;
  // JSON parse
  auto s = Settings::from_json(Json{{"log_level","debug"},{"enable_rich_tracebacks",true}});
  assert(s.log_level == "debug");
  assert(s.enable_rich_tracebacks == true);

  // Env parse (set locally)
  set_env("FASTMCPP_LOG_LEVEL","warn");
  set_env("FASTMCPP_ENABLE_RICH_TRACEBACKS","1");
  auto e = Settings::from_env();
  assert(e.log_level == "WARN"); // uppercased
  assert(e.enable_rich_tracebacks == true);
  return 0;
}

