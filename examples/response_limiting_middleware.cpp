#include "fastmcpp/server/response_limiting_middleware.hpp"
#include "fastmcpp/server/server.hpp"

#include <iostream>
#include <string>

using namespace fastmcpp;

int main()
{
    server::Server srv("response_limiting_demo", "1.0.0");

    srv.route("tools/call",
              [](const Json& payload)
              {
                  Json args = payload.value("arguments", Json::object());
                  std::string text = args.value("text", std::string(120, 'A'));
                  return Json{
                      {"content", Json::array({{{"type", "text"}, {"text", text}}})},
                  };
              });

    server::ResponseLimitingMiddleware limiter(48, "... [truncated]");
    srv.add_after(limiter.make_hook());

    Json req = {{"name", "echo_large"},
                {"arguments",
                 {{"text",
                   "This response is intentionally long so middleware truncation is easy to see."}}}};

    auto resp = srv.handle("tools/call", req);
    std::cout << resp.dump(2) << "\n";
    return 0;
}
