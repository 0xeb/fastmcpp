#include <cassert>
#include "fastmcpp/prompts/prompt.hpp"
#include "fastmcpp/prompts/manager.hpp"

int main() {
  using namespace fastmcpp;
  prompts::Prompt p{"Hello {name}!"};
  auto out = p.render({{"name","World"}});
  assert(out == "Hello World!");

  prompts::PromptManager pm;
  pm.add("greet", p);
  assert(pm.has("greet"));
  auto out2 = pm.get("greet").render({{"name","Ada"}});
  assert(out2 == "Hello Ada!");
  return 0;
}

