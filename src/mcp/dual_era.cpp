#include "fastmcpp/mcp/dual_era.hpp"

#include "fastmcpp/mcp/input_required.hpp"
#include "fastmcpp/mcp/subscriptions.hpp"
#include "fastmcpp/server/subscriptions.hpp"

#include <set>

namespace fastmcpp::mcp
{

namespace
{

// PYTHON REFERENCE: mcp_types/methods.py:113-124 vs the handshake request set.
// Requests that exist only on the handshake era; the sessionless wire answers
// them with method-not-found.
const std::set<std::string>& handshake_only_methods()
{
    static const std::set<std::string> methods = {
        "initialize", "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe",
    };
    return methods;
}

// PYTHON REFERENCE: mcp_types/methods.py:411-425 (CacheableMethod)
bool is_cacheable(const std::string& method)
{
    return method == "prompts/list" || method == "resources/list" || method == "resources/read" ||
           method == "resources/templates/list" || method == "server/discover" ||
           method == "tools/list";
}

bool flag_true(const Json& object, const char* key)
{
    return object.contains(key) && object[key].is_boolean() && object[key].get<bool>();
}

// On the sessionless era change notifications are delivered only over
// subscriptions/listen, so listChanged/subscribe say whether that is served.
// The base handler reports it by setting them true on the modern-era identity;
// anything else reads as false.
// PYTHON REFERENCE: mcp/server/lowlevel/server.py:589-618
Json modern_capabilities(Json caps)
{
    if (!caps.is_object())
        return Json::object();
    caps.erase("logging");
    caps.erase("tasks");
    for (const char* key : {"tools", "prompts"})
        if (caps.contains(key) && caps[key].is_object())
            caps[key]["listChanged"] = flag_true(caps[key], "listChanged");
    if (caps.contains("resources") && caps["resources"].is_object())
    {
        caps["resources"]["listChanged"] = flag_true(caps["resources"], "listChanged");
        caps["resources"]["subscribe"] = flag_true(caps["resources"], "subscribe");
    }
    return caps;
}

// PYTHON REFERENCE: mcp_types/_types.py:1048-1050 (SubscriptionsListenRequestParams)
bool listen_params_valid(const Json& message)
{
    if (!message.contains("params") || !message["params"].is_object())
        return false;
    const Json& params = message["params"];
    return params.contains("notifications") &&
           validate_subscription_filter(params["notifications"]).has_value();
}

/// @p private_meta holds server-side facts (negotiated version, routing
/// headers) for the base handler; they are merged into `params._meta`.
Json with_era(const Json& message, const char* era, const Json& private_meta = Json::object())
{
    Json out = message;
    if (!out.contains("params") || !out["params"].is_object())
        out["params"] = Json::object();
    Json& params = out["params"];
    if (!params.contains("_meta") || !params["_meta"].is_object())
        params["_meta"] = Json::object();
    // Server-private routing facts are set here, never taken from the client.
    for (auto it = params["_meta"].begin(); it != params["_meta"].end();)
        it = it.key().rfind("fastmcpp/", 0) == 0 ? params["_meta"].erase(it) : std::next(it);
    params["_meta"][kEraMetaKey] = era;
    for (const auto& [key, value] : private_meta.items())
        params["_meta"][key] = value;

    // Tools build their Context from arguments._meta, so the era and the
    // request's protocol version travel there too; that is what lets
    // Context::elicit()/sample() refuse on the sessionless era.
    if (out.value("method", std::string()) == "tools/call" && std::string(era) == "modern")
    {
        if (!params.contains("arguments") || !params["arguments"].is_object())
            params["arguments"] = Json::object();
        Json& args = params["arguments"];
        if (!args.contains("_meta") || !args["_meta"].is_object())
            args["_meta"] = Json::object();
        args["_meta"][kEraMetaKey] = era;
        if (params["_meta"].contains(kProtocolVersionMetaKey))
            args["_meta"][kProtocolVersionMetaKey] = params["_meta"][kProtocolVersionMetaKey];
    }
    return out;
}

/// Hand a retry's input to the handler where it can read it: tools and prompts
/// get only their arguments, resources get the whole read params.
void deliver_input(Json& message, const std::string& method, const Json* responses,
                   const std::optional<std::string>& state)
{
    if (!responses && !state)
        return;
    Json& params = message["params"];
    Json* holder = &params;
    if (method != "resources/read")
    {
        if (!params.contains("arguments") || !params["arguments"].is_object())
            params["arguments"] = Json::object();
        holder = &params["arguments"];
    }
    if (!holder->contains("_meta") || !(*holder)["_meta"].is_object())
        (*holder)["_meta"] = Json::object();
    if (responses)
        (*holder)["_meta"][kInputResponsesMetaKey] = *responses;
    if (state)
        (*holder)["_meta"][kRequestStateMetaKey] = *state;
}

Json invalid_request_state(const Json& id)
{
    // PYTHON REFERENCE: mcp/server/request_state.py:268-277 (_reject)
    return make_error_response(id, kInvalidParams, "Invalid or expired requestState",
                               Json{{"reason", "invalid_request_state"}});
}

/// The handshake-era refusal for a handler that asked for input.
// PYTHON REFERENCE: fastmcp/server/mixins/mcp_operations.py:344-357, 425-438, 467-480
Json input_required_on_handshake(const Json& id, const std::string& method, const Json& params,
                                 const std::optional<std::string>& negotiated)
{
    const std::string version = negotiated ? "'" + *negotiated + "'" : "None";
    const std::string tail =
        "only exists at MCP 2026-07-28; this connection negotiated " + version + ".";
    std::string message;
    if (method == "tools/call")
        message = "Tool '" + params.value("name", std::string()) +
                  "' returned an InputRequiredResult to request client input, but the "
                  "multi-round-trip result type (SEP-2322) " +
                  tail +
                  " Use ctx.elicit() for server-initiated input on handshake-era connections.";
    else if (method == "resources/read")
        message = "Resource '" + params.value("uri", std::string()) +
                  "' returned an InputRequiredResult to request client input, but the "
                  "multi-round-trip result type (SEP-2322) " +
                  tail;
    else
        message = "Prompt '" + params.value("name", std::string()) +
                  "' returned an InputRequiredResult to request client input, but the "
                  "multi-round-trip result type (SEP-2322) " +
                  tail;
    return make_error_response(id, kInvalidParams, message);
}

bool reply_is_input_required(const Json& reply)
{
    return reply.is_object() && reply.contains("result") && is_input_required(reply["result"]);
}

/// Run the base handler, turning a thrown InputRequired into its result.
Json call_base(const JsonRpcHandler& base, const Json& message)
{
    try
    {
        return base(message);
    }
    catch (const InputRequired& ir)
    {
        Json result = ir.result();
        if (!result.is_object())
            result = Json::object();
        result["resultType"] = "input_required";
        return Json{{"jsonrpc", "2.0"},
                    {"id", message.contains("id") ? message["id"] : Json()},
                    {"result", std::move(result)}};
    }
}

} // namespace

Json make_error_response(const Json& id, int code, const std::string& message, const Json& data)
{
    Json error = {{"code", code}, {"message", message}};
    if (!data.is_null())
        error["data"] = data;
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}

std::string wire_version_of_request(const Json& params)
{
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object())
        return {};
    const Json& meta = params["_meta"];
    const char* key = era_of_request(params) == protocol::Era::Modern ? kProtocolVersionMetaKey
                                                                      : kNegotiatedVersionMetaKey;
    if (meta.contains(key) && meta[key].is_string())
        return meta[key].get<std::string>();
    return {};
}

protocol::Era era_of_request(const Json& params)
{
    if (params.is_object() && params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].value(kEraMetaKey, std::string()) == "modern")
        return protocol::Era::Modern;
    return protocol::Era::Handshake;
}

DualEraHandler::DualEraHandler(JsonRpcHandler base,
                               std::shared_ptr<const RequestStateSecurity> request_state)
    : base_(std::move(base)), request_state_(std::move(request_state))
{
}

Json DualEraHandler::server_identity() const
{
    // Ask the base handler the way a handshake client would: it already knows
    // the server's name, version, capabilities and instructions, for every
    // make_mcp_handler variant.
    // Marked modern so the base handler includes what only the sessionless
    // era advertises (registered extensions).
    Json init = {{"jsonrpc", "2.0"},
                 {"id", "fastmcpp-dual-era-identity"},
                 {"method", "initialize"},
                 {"params",
                  {{"protocolVersion", protocol::kLatestHandshake},
                   {"capabilities", Json::object()},
                   {"clientInfo", {{"name", "fastmcpp-dual-era"}, {"version", "0"}}},
                   {"_meta", {{kEraMetaKey, "modern"}}}}}};
    Json reply = base_(init);
    if (reply.is_object() && reply.contains("result") && reply["result"].is_object())
        return reply["result"];
    return Json::object();
}

Json DualEraHandler::discover(const Json& id, const Envelope&) const
{
    // PYTHON REFERENCE: mcp/server/lowlevel/server.py:673-686 (_handle_discover)
    Json identity = server_identity();
    Json supported = Json::array();
    for (const auto& v : protocol::kModernVersions)
        supported.push_back(std::string(v));
    Json result = {
        {"supportedVersions", supported},
        {"capabilities", modern_capabilities(identity.value("capabilities", Json::object()))}};
    if (identity.contains(kCacheHintKey))
        result[kCacheHintKey] = identity[kCacheHintKey];
    if (identity.contains("instructions") && identity["instructions"].is_string())
        result["instructions"] = identity["instructions"];
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

Json DualEraHandler::handle_handshake(const Json& message,
                                      const std::optional<std::string>& negotiated_version) const
{
    const std::string method = message.value("method", std::string());
    const bool is_request = message.contains("id") && !message["id"].is_null();
    // PYTHON REFERENCE: mcp/server/runner.py:~710-714
    if (is_request && method != "initialize" && message.contains("params") &&
        has_modern_envelope(message["params"]))
        return make_error_response(message["id"], kInvalidRequest,
                                   "this connection serves the handshake protocol era; requests "
                                   "carrying the 2026-07-28 envelope are not accepted on it");
    // subscriptions/listen exists only at 2026-07-28.
    // PYTHON REFERENCE: mcp/server/runner.py:193-197
    if ((method == "server/discover" || method == "subscriptions/listen") && is_request)
        return make_error_response(message["id"], kMethodNotFound, "Method not found", method);
    Json private_meta = Json::object();
    if (negotiated_version)
        private_meta[kNegotiatedVersionMetaKey] = *negotiated_version;
    Json reply = call_base(base_, with_era(message, "handshake", private_meta));
    if (is_request && is_input_required_method(method) && reply_is_input_required(reply))
        return input_required_on_handshake(
            message["id"], method, message.value("params", Json::object()), negotiated_version);
    return reply;
}

// PYTHON REFERENCE: mcp/server/request_state.py:375-386 (RequestStateBoundary.__call__)
Json DualEraHandler::call_input_required_method(const Json& message, const Json& id,
                                                const std::string& method,
                                                const Json& private_meta) const
{
    const Json params = message.value("params", Json::object());
    Json server_info = server_identity().value("serverInfo", Json::object());
    std::optional<std::string> name;
    if (server_info.contains("name") && server_info["name"].is_string())
        name = server_info["name"].get<std::string>();
    RequestStateBoundary boundary(request_state_, name);

    // An explicit JSON null counts as absent.
    std::optional<std::string> plaintext;
    if (params.contains("requestState") && !params["requestState"].is_null())
    {
        plaintext = boundary.unseal(method, params);
        if (!plaintext)
            return invalid_request_state(id);
    }

    Json routed = with_era(message, "modern", private_meta);
    if (plaintext)
        routed["params"]["requestState"] = *plaintext;
    const Json* responses = nullptr;
    if (params.contains("inputResponses") && params["inputResponses"].is_object())
        responses = &params["inputResponses"];
    deliver_input(routed, method, responses, plaintext);

    Json reply = call_base(base_, routed);
    if (!reply_is_input_required(reply))
        return reply;

    // PYTHON REFERENCE: mcp_types/_types.py:2093-2097 (at least one field)
    Json& result = reply["result"];
    const bool has_requests = result.contains("inputRequests") &&
                              result["inputRequests"].is_object() &&
                              !result["inputRequests"].empty();
    const bool has_state = result.contains("requestState") && !result["requestState"].is_null();
    if (!has_requests && !has_state)
        return make_error_response(id, kInternalError, "Handler returned an invalid result");
    if (has_state)
    {
        if (!result["requestState"].is_string())
            return make_error_response(id, kInternalError, "Handler returned an invalid result");
        try
        {
            // Bound to the request as the client sent it, before any injection.
            result["requestState"] =
                boundary.seal(method, params, result["requestState"].get<std::string>());
        }
        catch (const std::exception&)
        {
            return make_error_response(id, kInternalError, "Internal error");
        }
    }
    return reply;
}

Json DualEraHandler::handle_modern(const Json& message, const HeaderMap* headers) const
{
    const Json id = message.contains("id") ? message["id"] : Json();
    const std::string method = message.value("method", std::string());

    // Notifications carry no envelope; the only client notification on this
    // era is notifications/cancelled, and a stateless server has nothing to
    // cancel by a client-chosen id, so it is acknowledged and dropped.
    // PYTHON REFERENCE: mcp/server/_streamable_http_modern.py:224-251
    if (!message.contains("id"))
        return Json();

    auto verdict = classify_request(message, headers);
    if (auto* rej = std::get_if<EnvelopeRejection>(&verdict))
        return make_error_response(id, rej->code, rej->message, rej->data);
    const Envelope& env = std::get<Envelope>(verdict);

    Json private_meta = Json::object();
    if (headers != nullptr)
    {
        auto name = headers->find(kNameHeader);
        if (name != headers->end())
            private_meta[kNameHeaderMetaKey] = name->second;
    }

    Json reply;
    if (method == "server/discover")
        reply = discover(id, env);
    else if (handshake_only_methods().count(method))
        // PYTHON REFERENCE: mcp/server/runner.py:197-210
        reply = make_error_response(id, kMethodNotFound, "Method not found", method);
    else if (method == "subscriptions/listen")
    {
        // PYTHON REFERENCE: mcp/server/runner.py:193-197 (validate_client_request)
        if (!listen_params_valid(message))
            return make_error_response(id, kInvalidParams, "Invalid request parameters", "");
        server::ListenStreamScope scope(listen_streams_);
        reply = base_(with_era(message, "modern", private_meta));
        // A stream hand-off is the transport's to serve, not a result to shape.
        if (reply.is_object() && reply.contains("result") && reply["result"].is_object() &&
            reply["result"].contains(server::kListenStreamResultKey))
            return reply;
    }
    else if (is_input_required_method(method))
        reply = call_input_required_method(message, id, method, private_meta);
    else
        reply = base_(with_era(message, "modern", private_meta));

    if (!reply.is_object() || !reply.contains("result") || !reply["result"].is_object())
        return reply;

    // PYTHON REFERENCE: mcp/server/runner.py:340-417 (_serialize, _stamp_server_info)
    Json& result = reply["result"];
    Json identity = server_identity();
    // Interim input_required results never carry cache hints. A field the
    // handler set wins over the server-wide hint, per field.
    // PYTHON REFERENCE: mcp/server/caching.py:47-61 (apply_cache_hint)
    if (is_cacheable(method) && !is_input_required(result))
    {
        Json hint = identity.value(kCacheHintKey, Json::object());
        if (!result.contains("ttlMs"))
            result["ttlMs"] = hint.value("ttlMs", 0);
        if (!result.contains("cacheScope"))
            result["cacheScope"] = hint.value("cacheScope", std::string("private"));
    }
    result.erase(kCacheHintKey);
    if (!result.contains("resultType") || result["resultType"].is_null())
        result["resultType"] = "complete";

    Json server_info = identity.value("serverInfo", Json::object());
    if (!result.contains("_meta") || result["_meta"].is_null())
        result["_meta"] = Json{{kServerInfoMetaKey, server_info}};
    else if (result["_meta"].is_object() && (!result["_meta"].contains(kServerInfoMetaKey) ||
                                             result["_meta"][kServerInfoMetaKey].is_null()))
        result["_meta"][kServerInfoMetaKey] = server_info;
    return reply;
}

Json DualEraHandler::operator()(const Json& message) const
{
    const std::string method = message.value("method", std::string());
    if (method != "initialize" && message.contains("params") &&
        has_modern_envelope(message["params"]))
        return handle_modern(message);
    return handle_handshake(message);
}

} // namespace fastmcpp::mcp
