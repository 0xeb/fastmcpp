#include "fastmcpp/server/sessions.hpp"

#include "fastmcpp/tools/tool.hpp"
#include "fastmcpp/util/crypto.hpp"

#include <chrono>
#include <cstdio>

namespace fastmcpp::server
{

namespace
{
// PYTHON REFERENCE: fastmcp/server/sessions.py:91-101
constexpr const char* kMarkerKey = "_created";
constexpr const char* kStateKey = "state";
constexpr const char* kUserSessionId = "_user";

std::string hex(const std::array<std::uint8_t, 32>& digest)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : digest)
    {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

std::string uuid4()
{
    auto bytes = util::crypto::random_bytes(16);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                  bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                  bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

Json state_of(const std::optional<Json>& raw)
{
    if (!raw || !raw->is_object())
        return Json::object();
    auto it = raw->find(kStateKey);
    return it != raw->end() && it->is_object() ? *it : Json::object();
}

Json text_result(const std::string& text, bool is_error)
{
    Json out = {{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}};
    if (is_error)
        out["isError"] = true;
    else
        out["structuredContent"] = Json{{"result", text}};
    return out;
}
} // namespace

const char* const kSessionIdDescription =
    "Session identifier. Use a tool to create a session, then pass the resulting id here to "
    "persist state across calls in the same session.";

Json session_id_property()
{
    return Json{{"type", "string"}, {"description", kSessionIdDescription}};
}

// -- store ------------------------------------------------------------------

std::optional<Json> InMemorySessionStateStore::get(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it == values_.end())
        return std::nullopt;
    return it->second;
}

void InMemorySessionStateStore::put(const std::string& key, const Json& value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
}

void InMemorySessionStateStore::remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    values_.erase(key);
}

std::string session_storage_key(const std::optional<std::string>& principal,
                                const std::string& session_id)
{
    const std::string segment = principal ? hex(util::crypto::sha256(*principal)) : "anon";
    return "session:" + segment + ":" + session_id;
}

// -- session ----------------------------------------------------------------

Session::Session(std::shared_ptr<SessionStateStore> store, std::optional<std::string> principal,
                 std::string session_id, std::optional<std::string> public_id)
    : store_(std::move(store)), public_id_(std::move(public_id)),
      key_(session_storage_key(principal, session_id))
{
}

std::optional<Json> Session::load_raw() const
{
    auto raw = store_->get(key_);
    if (!raw || !raw->is_object())
        return std::nullopt;
    return raw;
}

void Session::save_raw(const Json& raw)
{
    store_->put(key_, raw);
}

bool Session::exists() const
{
    auto raw = load_raw();
    return raw && raw->contains(kMarkerKey);
}

void Session::create()
{
    Json raw = load_raw().value_or(Json::object());
    raw[kMarkerKey] =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!raw.contains(kStateKey))
        raw[kStateKey] = Json::object();
    save_raw(raw);
}

Json Session::get(const std::string& key, const Json& default_value) const
{
    Json state = state_of(load_raw());
    auto it = state.find(key);
    return it != state.end() ? *it : default_value;
}

void Session::set(const std::string& key, const Json& value)
{
    auto loaded = load_raw();
    Json raw = loaded.value_or(Json::object());
    Json state = state_of(loaded);
    state[key] = value;
    raw[kStateKey] = std::move(state);
    save_raw(raw);
}

void Session::remove(const std::string& key)
{
    auto raw = load_raw();
    if (!raw)
        return;
    Json state = state_of(raw);
    if (state.erase(key) == 0)
        return;
    (*raw)[kStateKey] = std::move(state);
    save_raw(*raw);
}

void Session::clear()
{
    auto raw = load_raw();
    if (!raw)
        return;
    (*raw)[kStateKey] = Json::object();
    save_raw(*raw);
}

void Session::end()
{
    store_->remove(key_);
}

// -- resolution -------------------------------------------------------------

std::optional<Session> optional_user_session(const std::shared_ptr<SessionStateStore>& store)
{
    auto principal = current_principal();
    if (!principal)
        return std::nullopt;
    return Session(store, principal, kUserSessionId);
}

Session user_session(const std::shared_ptr<SessionStateStore>& store)
{
    auto session = optional_user_session(store);
    if (!session)
        throw SessionAuthError();
    return std::move(*session);
}

std::string create_session(const std::shared_ptr<SessionStateStore>& store)
{
    const std::string id = uuid4();
    Session(store, current_principal(), id, id).create();
    return id;
}

Session get_session(const std::shared_ptr<SessionStateStore>& store, const std::string& session_id)
{
    Session session(store, current_principal(), session_id, session_id);
    if (!session.exists())
        throw InvalidSession();
    return session;
}

// -- provider ---------------------------------------------------------------

SessionProvider::SessionProvider(StoreGetter store) : store_(std::move(store)) {}

std::vector<tools::Tool> SessionProvider::list_tools() const
{
    // PYTHON REFERENCE: fastmcp/server/sessions.py:376-400 (create_session, end_session)
    const Json output_schema = Json{{"type", "string"}};
    auto store = store_;

    tools::Tool create(
        "create_session",
        Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
        output_schema, [store](const Json&) -> Json
        { return text_result(create_session(store()), false); }, std::string("Create Session"),
        std::string(
            "Create a new session and return its identifier.\n\nMints an unguessable `uuid4`, "
            "records an initial session owned by the current\nprincipal, and returns the id as a "
            "string. Store it and pass it back as a\n`session_id` argument on later calls to "
            "persist state across a session \xE2\x80\x94 only\nan id created this way resolves. "
            "State is "
            "keyed by the authenticated\nprincipal, so the id organizes sessions within a user; on "
            "an unauthenticated\nconnection the id is the only thing standing between callers, "
            "which is why it\nis unguessable."),
        std::nullopt);

    tools::Tool end(
        "end_session",
        Json{{"type", "object"},
             {"additionalProperties", false},
             {"properties", Json{{"session_id", session_id_property()}}},
             {"required", Json::array({"session_id"})}},
        output_schema,
        [store](const Json& args) -> Json
        {
            const auto& id = args.contains("session_id") && args["session_id"].is_string()
                                 ? args["session_id"].get<std::string>()
                                 : std::string();
            try
            {
                get_session(store(), id).end();
            }
            catch (const InvalidSession& e)
            {
                return text_result(e.what(), true);
            }
            return text_result("session ended", false);
        },
        std::string("End Session"),
        std::string("End a session and delete all of its state.\n\nValidates the id like any other "
                    "resolution (an unknown or foreign id is\nrejected), then deletes the "
                    "session's key so the id no longer resolves."),
        std::nullopt);

    return {std::move(create), std::move(end)};
}

} // namespace fastmcpp::server
