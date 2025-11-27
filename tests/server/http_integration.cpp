#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/server/http_server.hpp"
#include "fastmcpp/server/server.hpp"

#include <cassert>
#include <chrono>
#include <thread>

int main()
{
    using namespace fastmcpp;
    auto srv = std::make_shared<server::Server>();
    srv->route("sum", [](const Json& j) { return j.at("a").get<int>() + j.at("b").get<int>(); });

    const int port = 18081;
    server::HttpServerWrapper http{srv, "127.0.0.1", port};
    bool ok = http.start();
    assert(ok);
    // Give server a moment (already waits briefly in start)
    client::HttpTransport http_client{"127.0.0.1:" + std::to_string(port)};
    auto out = http_client.request("sum", Json{{"a", 10}, {"b", 7}});
    assert(out.get<int>() == 17);
    http.stop();
    return 0;
}
