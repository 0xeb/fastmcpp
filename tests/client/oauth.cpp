// Client OAuth (include/fastmcpp/client/auth.hpp) against an in-process authorization server
// and protected MCP endpoint.
//
// PYTHON REFERENCE: tests/client/auth/test_oauth_client.py, test_oauth_static_client.py,
// test_oauth_cimd.py, and the MCP SDK's tests for OAuthClientProvider.

#include "fastmcpp/client/auth.hpp"
#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using fastmcpp::Json;
namespace auth = fastmcpp::client::auth;

namespace
{

std::map<std::string, std::string> parse_form(const std::string& body)
{
    httplib::Params params;
    httplib::detail::parse_query_text(body, params);
    return {params.begin(), params.end()};
}

/// An authorization server (RFC 8414, 7591, 6749 + PKCE) and an MCP resource server, each on
/// its own loopback port.
class FakeServers
{
  public:
    // knobs
    std::string issuer_override; // non-empty: metadata advertises this issuer instead
    bool unknown_client_400 = true;
    long long expires_in = 3600;
    bool omit_rotated_refresh_token = false;
    std::string required_scope; // non-empty: the RS demands it with a 403 step-up
    std::set<std::string> known_clients;

    // observations
    std::atomic<int> registrations{0};
    std::atomic<int> authorizations{0};
    std::atomic<int> token_requests{0};
    std::atomic<int> refreshes{0};
    std::atomic<int> unauthorized{0};
    std::atomic<int> mcp_ok{0};
    std::mutex mu;
    Json last_registration;
    std::map<std::string, std::string> last_authorize;
    std::map<std::string, std::string> last_token_form;

    FakeServers()
    {
        as_port_ = as_.bind_to_any_port("127.0.0.1");
        rs_port_ = rs_.bind_to_any_port("127.0.0.1");
        setup_as();
        setup_rs();
        as_thread_ = std::thread([this] { as_.listen_after_bind(); });
        rs_thread_ = std::thread([this] { rs_.listen_after_bind(); });
        as_.wait_until_ready();
        rs_.wait_until_ready();
    }

    ~FakeServers()
    {
        as_.stop();
        rs_.stop();
        as_thread_.join();
        rs_thread_.join();
    }

    std::string as_url() const
    {
        return "http://127.0.0.1:" + std::to_string(as_port_);
    }
    std::string rs_base() const
    {
        return "http://127.0.0.1:" + std::to_string(rs_port_);
    }
    std::string mcp_url() const
    {
        return rs_base() + "/mcp";
    }

    void revoke_all_tokens()
    {
        std::lock_guard<std::mutex> lock(mu);
        valid_tokens_.clear();
    }

  private:
    void setup_as()
    {
        as_.Get("/.well-known/oauth-authorization-server",
                [this](const httplib::Request&, httplib::Response& res)
                {
                    Json m = {{"issuer", issuer_override.empty() ? as_url() : issuer_override},
                              {"authorization_endpoint", as_url() + "/authorize"},
                              {"token_endpoint", as_url() + "/token"},
                              {"registration_endpoint", as_url() + "/register"},
                              {"code_challenge_methods_supported", Json::array({"S256"})}};
                    res.set_content(m.dump(), "application/json");
                });
        as_.Post("/register",
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     const int n = ++registrations;
                     const std::string id = "client-" + std::to_string(n);
                     {
                         std::lock_guard<std::mutex> lock(mu);
                         last_registration = Json::parse(req.body);
                         known_clients.insert(id);
                     }
                     Json info = last_registration;
                     info["client_id"] = id;
                     info["client_secret"] = "secret-" + std::to_string(n);
                     info["token_endpoint_auth_method"] = "client_secret_post";
                     info["issuer"] = "https://forged.example/"; // must be ignored by the client
                     res.status = 201;
                     res.set_content(info.dump(), "application/json");
                 });
        as_.Get("/authorize",
                [this](const httplib::Request& req, httplib::Response& res)
                {
                    std::map<std::string, std::string> q(req.params.begin(), req.params.end());
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        last_authorize = q;
                        if (unknown_client_400 && !known_clients.count(q["client_id"]))
                        {
                            res.status = 400;
                            return;
                        }
                        codes_["code-" + q["state"]] = {q["code_challenge"], q["scope"]};
                    }
                    ++authorizations;
                    res.status = 302;
                    res.set_header("Location", q["redirect_uri"] + "?code=code-" + q["state"] +
                                                   "&state=" + q["state"]);
                });
        as_.Post("/token",
                 [this](const httplib::Request& req, httplib::Response& res)
                 {
                     ++token_requests;
                     auto form = parse_form(req.body);
                     std::lock_guard<std::mutex> lock(mu);
                     last_token_form = form;
                     std::string token;
                     std::string scope;
                     long long ttl = 3600;
                     if (form["grant_type"] == "authorization_code")
                     {
                         auto it = codes_.find(form["code"]);
                         if (it == codes_.end() || auth::detail::pkce_challenge(
                                                       form["code_verifier"]) != it->second.first)
                         {
                             res.status = 400;
                             res.set_content(R"({"error":"invalid_grant"})", "application/json");
                             return;
                         }
                         scope = it->second.second;
                         codes_.erase(it);
                         token = "access-" + std::to_string(++issued_);
                         ttl = expires_in;
                     }
                     else if (form["grant_type"] == "refresh_token" &&
                              refresh_tokens_.count(form["refresh_token"]))
                     {
                         ++refreshes;
                         scope = refresh_scopes_[form["refresh_token"]];
                         token = "refreshed-" + std::to_string(++issued_);
                     }
                     else
                     {
                         res.status = 400;
                         res.set_content(R"({"error":"invalid_grant"})", "application/json");
                         return;
                     }
                     valid_tokens_.insert(token);
                     token_scopes_[token] = scope;
                     const std::string refresh = "refresh-" + std::to_string(issued_);
                     refresh_tokens_.insert(refresh);
                     refresh_scopes_[refresh] = scope;
                     Json t = {{"access_token", token},
                               {"token_type", "bearer"},
                               {"expires_in", ttl},
                               {"refresh_token", refresh},
                               {"scope", scope}};
                     if (omit_rotated_refresh_token && form["grant_type"] == "refresh_token")
                         t.erase("refresh_token");
                     res.set_content(t.dump(), "application/json");
                 });
    }

    void setup_rs()
    {
        rs_.Get("/.well-known/oauth-protected-resource/mcp",
                [this](const httplib::Request&, httplib::Response& res)
                {
                    Json prm = {{"resource", mcp_url()},
                                {"authorization_servers", Json::array({as_url()})},
                                {"scopes_supported", Json::array({"read", "write"})}};
                    res.set_content(prm.dump(), "application/json");
                });
        rs_.Post(
            "/mcp",
            [this](const httplib::Request& req, httplib::Response& res)
            {
                const std::string authz = req.get_header_value("Authorization");
                const std::string prm = rs_base() + "/.well-known/oauth-protected-resource/mcp";
                bool ok = false;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    ok = authz.rfind("Bearer ", 0) == 0 && valid_tokens_.count(authz.substr(7));
                }
                if (!ok)
                {
                    ++unauthorized;
                    res.status = 401;
                    res.set_header("WWW-Authenticate", "Bearer resource_metadata=\"" + prm + "\"");
                    return;
                }
                if (!required_scope.empty())
                {
                    std::lock_guard<std::mutex> lock(mu);
                    if (token_scopes_[authz.substr(7)].find(required_scope) == std::string::npos)
                    {
                        res.status = 403;
                        res.set_header("WWW-Authenticate",
                                       "Bearer error=\"insufficient_scope\", scope=\"" +
                                           required_scope + "\", resource_metadata=\"" + prm +
                                           "\"");
                        return;
                    }
                }
                ++mcp_ok;
                Json rpc = Json::parse(req.body);
                Json reply = {{"jsonrpc", "2.0"},
                              {"id", rpc.value("id", Json())},
                              {"result", Json{{"pong", true}}}};
                res.set_content(reply.dump(), "application/json");
            });
    }

    httplib::Server as_, rs_;
    int as_port_ = 0, rs_port_ = 0;
    std::thread as_thread_, rs_thread_;
    std::map<std::string, std::pair<std::string, std::string>> codes_; // code -> {challenge, scope}
    std::map<std::string, std::string> token_scopes_, refresh_scopes_;
    std::set<std::string> valid_tokens_, refresh_tokens_;
    int issued_ = 0;
};

/// A headless browser: follow the authorization redirect and hand {code, state} back.
struct HeadlessBrowser
{
    std::pair<std::string, std::optional<std::string>> result;
    std::function<void(const std::string&)> redirect_handler()
    {
        return [this](const std::string& url)
        {
            const auto q = url.find('?');
            const std::string origin_path = url.substr(0, q);
            const auto scheme_end = origin_path.find("://") + 3;
            const auto path_start = origin_path.find('/', scheme_end);
            httplib::Client cli(origin_path.substr(0, path_start));
            auto res = cli.Get(origin_path.substr(path_start) + url.substr(q));
            assert(res);
            if (res->status == 400)
                throw auth::ClientNotFoundError("authorization server does not know the client");
            assert(res->status == 302);
            const std::string location = res->get_header_value("Location");
            httplib::Params p;
            httplib::detail::parse_query_text(location.substr(location.find('?') + 1), p);
            result.first = p.find("code")->second;
            result.second = p.find("state")->second;
        };
    }
    std::function<std::pair<std::string, std::optional<std::string>>()> callback_handler()
    {
        return [this] { return result; };
    }
};

std::unique_ptr<fastmcpp::client::StreamableHttpTransport> transport_for(const FakeServers& s)
{
    return std::make_unique<fastmcpp::client::StreamableHttpTransport>(s.rs_base(), "/mcp");
}

void test_helpers()
{
    std::cout << "test_helpers..." << std::flush;
    // RFC 7636 appendix B
    assert(auth::detail::pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
           "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    assert(auth::detail::sha256_hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(auth::detail::sha256_hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string www =
        R"(Bearer error="insufficient_scope", scope="a b", resource_metadata=https://x/y)";
    assert(auth::detail::www_authenticate_field(www, "scope") == "a b");
    assert(auth::detail::www_authenticate_field(www, "resource_metadata") == "https://x/y");
    assert(!auth::detail::www_authenticate_field(www, "realm"));
    assert(auth::detail::issuers_match("https://as.example", "https://as.example/"));
    assert(!auth::detail::issuers_match("https://as.example/a", "https://as.example/a/"));
    assert(auth::detail::check_resource_allowed("https://h/api/v1", "https://h/api"));
    assert(!auth::detail::check_resource_allowed("https://h/api123", "https://h/api"));
    assert(!auth::detail::check_resource_allowed("https://h:8443/api", "https://h/api"));
    const auto prm = auth::detail::protected_resource_discovery_urls(std::nullopt, "https://h/mcp");
    assert((prm == std::vector<std::string>{"https://h/.well-known/oauth-protected-resource/mcp",
                                            "https://h/.well-known/oauth-protected-resource"}));
    const auto asm_ = auth::detail::authorization_server_discovery_urls(
        std::string("https://as/tenant1/"), "https://h/mcp");
    assert((asm_ ==
            std::vector<std::string>{"https://as/.well-known/oauth-authorization-server/tenant1",
                                     "https://as/.well-known/openid-configuration/tenant1",
                                     "https://as/tenant1/.well-known/openid-configuration"}));
    auth::BearerAuth bearer("t0k");
    assert(bearer.headers({})["Authorization"] == "Bearer t0k");
    std::cout << " OK\n";
}

void test_full_flow_and_token_reuse()
{
    std::cout << "test_full_flow_and_token_reuse..." << std::flush;
    FakeServers s;
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto oauth = std::make_shared<auth::OAuth>(o);
    auto t = transport_for(s);
    t->set_auth(oauth);

    auto r = t->request("ping", Json::object());
    assert(r["pong"] == true);
    assert(s.unauthorized == 1 && s.registrations == 1 && s.authorizations == 1);
    {
        std::lock_guard<std::mutex> lock(s.mu);
        assert(s.last_authorize["code_challenge_method"] == "S256");
        assert(s.last_authorize["resource"] == s.mcp_url()); // RFC 8707
        assert(s.last_authorize["scope"] == "read write");   // from PRM scopes_supported
        assert(s.last_authorize["redirect_uri"] == oauth->redirect_uri());
        assert(s.last_token_form["client_secret"] == "secret-1"); // client_secret_post
        assert(s.last_registration["grant_types"] ==
               Json::array({"authorization_code", "refresh_token"}));
    }
    assert(oauth->current_token()->access_token == "access-1");

    // Sharing one provider with another endpoint must not disclose this server's token.
    auto other = std::make_unique<fastmcpp::client::StreamableHttpTransport>(
        "http://127.0.0.1:1", "/other");
    bool rebound = false;
    try
    {
        other->set_auth(oauth);
        rebound = true;
    }
    catch (const auth::OAuthError&)
    {
    }
    assert(!rebound);
    // Attaching to the original endpoint again remains supported.
    auto same = transport_for(s);
    same->set_auth(oauth);

    // The token is reused: no new flow.
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(s.unauthorized == 1 && s.authorizations == 1);
    std::cout << " OK\n";
}

void test_expired_token_refreshes()
{
    std::cout << "test_expired_token_refreshes..." << std::flush;
    FakeServers s;
    s.expires_in = -10; // already expired when issued
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto oauth = std::make_shared<auth::OAuth>(o);
    auto t = transport_for(s);
    t->set_auth(oauth);
    // First request: 401 -> flow -> token (expired) -> retry: headers() refreshes first.
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(s.refreshes >= 1);
    assert(oauth->current_token()->access_token.rfind("refreshed-", 0) == 0);
    std::cout << " OK\n";
}

void test_persistent_file_store()
{
    std::cout << "test_persistent_file_store..." << std::flush;
    FakeServers s;
    const auto path =
        (std::filesystem::temp_directory_path() / "fastmcpp_oauth_store_test.json").string();
    std::filesystem::remove(path);
    {
        HeadlessBrowser browser;
        auth::OAuthOptions o;
        o.redirect_handler = browser.redirect_handler();
        o.callback_handler = browser.callback_handler();
        o.token_storage = std::make_shared<auth::FileStore>(path);
        auto t = transport_for(s);
        t->set_auth(std::make_shared<auth::OAuth>(o));
        assert(t->request("ping", Json::object())["pong"] == true);
    }
    // A new client with the same store: tokens load, no browser flow.
    auth::OAuthOptions o2;
    o2.redirect_handler = [](const std::string&) { assert(!"no flow expected"); };
    o2.callback_handler = []() -> std::pair<std::string, std::optional<std::string>>
    { return {"", std::nullopt}; };
    o2.token_storage = std::make_shared<auth::FileStore>(path);
    auto t2 = transport_for(s);
    t2->set_auth(std::make_shared<auth::OAuth>(o2));
    assert(t2->request("ping", Json::object())["pong"] == true);
    assert(s.authorizations == 1);
    std::filesystem::remove(path);
    std::cout << " OK\n";
}

void test_reloaded_expired_token_rediscovers_authorization_server()
{
    std::cout << "test_reloaded_expired_token_rediscovers_authorization_server..." << std::flush;
    FakeServers s;
    auto store = std::make_shared<auth::MemoryStore>();
    {
        HeadlessBrowser browser;
        auth::OAuthOptions o;
        o.token_storage = store;
        o.redirect_handler = browser.redirect_handler();
        o.callback_handler = browser.callback_handler();
        auto t = transport_for(s);
        t->set_auth(std::make_shared<auth::OAuth>(o));
        assert(t->request("ping", Json::object())["pong"] == true);
    }
    store->put("mcp-oauth-token-expiry", s.mcp_url() + "/token_expiry",
               Json{{"expires_at", 0}});
    s.omit_rotated_refresh_token = true;
    auth::OAuthOptions o;
    o.token_storage = store;
    o.redirect_handler = [](const std::string&) { assert(!"refresh must not open a browser"); };
    const auto http = auth::default_http_function();
    o.http = [&](const auth::HttpRequest& request)
    {
        // The RS and AS are different origins. Never disclose refresh credentials to the RS.
        assert(request.method != "POST" || request.url == s.as_url() + "/token");
        return http(request);
    };
    auto t = transport_for(s);
    t->set_auth(std::make_shared<auth::OAuth>(o));
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(s.refreshes == 1);
    assert(s.authorizations == 1);
    assert(store->get("mcp-oauth-token", s.mcp_url() + "/tokens")->at("refresh_token") ==
           "refresh-1");
    // If AS discovery fails on a subsequent reload, fail before sending any credentials.
    store->put("mcp-oauth-token-expiry", s.mcp_url() + "/token_expiry",
               Json{{"expires_at", 0}});
    o.http = [&](const auth::HttpRequest& request)
    {
        assert(request.method == "GET");
        if (request.url.rfind(s.as_url(), 0) == 0)
        {
            fastmcpp::client::HttpResponse unavailable;
            unavailable.status = 404;
            return unavailable;
        }
        return http(request);
    };
    auto unavailable = transport_for(s);
    unavailable->set_auth(std::make_shared<auth::OAuth>(o));
    bool rejected = false;
    try
    {
        unavailable->request("ping", Json::object());
    }
    catch (const auth::OAuthFlowError&)
    {
        rejected = true;
    }
    assert(rejected);
    assert(s.refreshes == 1);
    std::cout << " OK\n";
}

void test_static_client_skips_registration()
{
    std::cout << "test_static_client_skips_registration..." << std::flush;
    FakeServers s;
    s.known_clients.insert("static-id");
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.client_id = "static-id";
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto t = transport_for(s);
    t->set_auth(std::make_shared<auth::OAuth>(o));
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(s.registrations == 0);
    std::lock_guard<std::mutex> lock(s.mu);
    assert(s.last_authorize["client_id"] == "static-id");
    assert(!s.last_token_form.count("client_secret")); // token_endpoint_auth_method "none"
    std::cout << " OK\n";
}

void test_stale_registration_is_replaced_once()
{
    std::cout << "test_stale_registration_is_replaced_once..." << std::flush;
    FakeServers s;
    auto store = std::make_shared<auth::MemoryStore>();
    // A cached registration the server no longer knows.
    store->put("mcp-oauth-client-info", s.mcp_url() + "/client_info",
               Json{{"client_id", "forgotten"}, {"token_endpoint_auth_method", "none"}});
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.token_storage = store;
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto t = transport_for(s);
    t->set_auth(std::make_shared<auth::OAuth>(o));
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(s.registrations == 1);
    std::cout << " OK\n";

    std::cout << "test_static_credentials_rejected..." << std::flush;
    FakeServers s2;
    HeadlessBrowser browser2;
    auth::OAuthOptions o2;
    o2.client_id = "unknown-static";
    o2.redirect_handler = browser2.redirect_handler();
    o2.callback_handler = browser2.callback_handler();
    auto t2 = transport_for(s2);
    t2->set_auth(std::make_shared<auth::OAuth>(o2));
    bool threw = false;
    try
    {
        t2->request("ping", Json::object());
    }
    catch (const auth::ClientNotFoundError& e)
    {
        threw = std::string(e.what()).find("static client credentials") != std::string::npos;
    }
    assert(threw);
    std::cout << " OK\n";
}

void test_issuer_mismatch_is_rejected()
{
    std::cout << "test_issuer_mismatch_is_rejected..." << std::flush;
    FakeServers s;
    s.issuer_override = "https://evil.example";
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto t = transport_for(s);
    t->set_auth(std::make_shared<auth::OAuth>(o));
    bool threw = false;
    try
    {
        t->request("ping", Json::object());
    }
    catch (const auth::OAuthFlowError& e)
    {
        threw = std::string(e.what()).find("issuer mismatch") != std::string::npos;
    }
    assert(threw);
    assert(s.registrations == 0); // nothing registered with a server that lied
    std::cout << " OK\n";
}

void test_scope_step_up()
{
    std::cout << "test_scope_step_up..." << std::flush;
    FakeServers s;
    s.required_scope = "admin";
    HeadlessBrowser browser;
    auth::OAuthOptions o;
    o.redirect_handler = browser.redirect_handler();
    o.callback_handler = browser.callback_handler();
    auto t = transport_for(s);
    t->set_auth(std::make_shared<auth::OAuth>(o));
    // 401 -> token with "read write" -> 403 insufficient_scope -> re-authorize with "admin".
    bool rejected = false;
    try
    {
        t->request("ping", Json::object());
    }
    catch (const fastmcpp::TransportHttpError& e)
    {
        rejected = e.status() == 403; // the first exchange retries once, then reports
    }
    if (rejected)
        assert(t->request("ping", Json::object())["pong"] == true);
    std::lock_guard<std::mutex> lock(s.mu);
    assert(s.last_authorize["scope"] == "admin");
    std::cout << " OK\n";
}

void test_default_loopback_callback()
{
    std::cout << "test_default_loopback_callback..." << std::flush;
    FakeServers s;
    auth::OAuthOptions o;
    o.callback_host = "127.0.0.1";
    // Act as the browser: GET the authorization URL, then follow the redirect to the
    // loopback callback server the default callback_handler runs.
    o.redirect_handler = [](const std::string& url)
    {
        std::thread(
            [url]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const auto q = url.find('?');
                const std::string base = url.substr(0, url.find('/', url.find("://") + 3));
                httplib::Client as(base);
                auto res = as.Get(url.substr(base.size()));
                const std::string location = res->get_header_value("Location");
                const std::string cb_base =
                    location.substr(0, location.find('/', location.find("://") + 3));
                httplib::Client cb(cb_base);
                auto done = cb.Get(location.substr(cb_base.size()));
                assert(done && done->status == 200);
                (void)q;
            })
            .detach();
    };
    auto oauth = std::make_shared<auth::OAuth>(o);
    auto t = transport_for(s);
    t->set_auth(oauth);
    assert(t->request("ping", Json::object())["pong"] == true);
    assert(oauth->redirect_uri().rfind("http://127.0.0.1:", 0) == 0);
    std::cout << " OK\n";
}

void test_check_if_auth_required()
{
    std::cout << "test_check_if_auth_required..." << std::flush;
    FakeServers s;
    httplib::Server open_server;
    open_server.Get("/mcp", [](const httplib::Request&, httplib::Response& res)
                    { res.set_content("ok", "text/plain"); });
    const int port = open_server.bind_to_any_port("127.0.0.1");
    std::thread th([&] { open_server.listen_after_bind(); });
    open_server.wait_until_ready();
    // The protected endpoint only accepts POST; GET answers 404 there, so probe the PRM-less path.
    assert(auth::check_if_auth_required("http://127.0.0.1:" + std::to_string(port) + "/mcp") ==
           false);
    assert(auth::check_if_auth_required("http://127.0.0.1:1/unreachable") == true);
    open_server.stop();
    th.join();
    std::cout << " OK\n";
}

} // namespace

int main()
{
    test_helpers();
    test_reloaded_expired_token_rediscovers_authorization_server();
    test_full_flow_and_token_reuse();
    test_expired_token_refreshes();
    test_persistent_file_store();
    test_static_client_skips_registration();
    test_stale_registration_is_replaced_once();
    test_issuer_mismatch_is_rejected();
    test_scope_step_up();
    test_default_loopback_callback();
    test_check_if_auth_required();
    std::cout << "All client OAuth tests passed\n";
    return 0;
}
