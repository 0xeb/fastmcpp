#pragma once
/// @file server/sessions.hpp
/// @brief Stateless session state (2026-07-28): per-user and per-session
///        storage keyed by the authenticated principal.
/// @details Modern connections keep no server-side state between requests.
///          Tools keep cross-call state explicitly, in the server's session
///          state store:
///          - the per-user bucket (`FastMCP::user_session()`), keyed by the
///            request's authenticated principal; requires auth;
///          - an agent-supplied session id minted by the `create_session` tool
///            (`SessionProvider`) and resolved with `FastMCP::get_session(id)`;
///            an id that was never created, or was created under another
///            principal, is rejected.
///          Isolation is the principal, not the id. The principal comes from
///          the bearer token a `TokenVerifier` accepted on the streamable HTTP
///          transport. Without one (stdio, static token, no auth) every caller
///          shares the anonymous principal, and a session id is just a bearer
///          capability.
// PYTHON REFERENCE: fastmcp/server/sessions.py

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/providers/provider.hpp"
#include "fastmcpp/server/auth_context.hpp"
#include "fastmcpp/types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmcpp::server
{

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

/// Backing store for session state: one JSON value per key. Retention (TTL,
/// eviction) is the store's concern; sessions never set one.
class SessionStateStore
{
  public:
    virtual ~SessionStateStore() = default;
    virtual std::optional<Json> get(const std::string& key) = 0;
    virtual void put(const std::string& key, const Json& value) = 0;
    virtual void remove(const std::string& key) = 0;
};

/// Process-local store (the default). Not shared across replicas.
class InMemorySessionStateStore : public SessionStateStore
{
  public:
    std::optional<Json> get(const std::string& key) override;
    void put(const std::string& key, const Json& value) override;
    void remove(const std::string& key) override;

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, Json> values_;
};

/// `session:<sha256(principal)|anon>:<session_id>`.
// PYTHON REFERENCE: fastmcp/server/sessions.py:147-170
std::string session_storage_key(const std::optional<std::string>& principal,
                                const std::string& session_id);

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

/// A per-user session was requested on an unauthenticated request.
struct SessionAuthError : Error
{
    SessionAuthError()
        : Error("Injected `session: UserSession` requires an authenticated principal, but "
                "this request is unauthenticated. Use a `session_id: SessionId` argument for "
                "cross-call state on unauthenticated connections.")
    {
    }
};

/// A session id did not resolve under the current principal. Deliberately
/// generic: unknown and foreign ids are indistinguishable to the caller.
struct InvalidSession : Error
{
    InvalidSession() : Error("Invalid or unknown session.") {}
};

/// Accessors over one `(principal, session_id)` bucket. User state lives in a
/// `state` sub-object beside a creation marker, so `clear()` keeps a created
/// session valid while `end()` deletes it. Writes read-modify-write the
/// stored object; concurrent writers to one session race, as upstream.
// PYTHON REFERENCE: fastmcp/server/sessions.py:173-300 (Session)
class Session
{
  public:
    Session(std::shared_ptr<SessionStateStore> store, std::optional<std::string> principal,
            std::string session_id, std::optional<std::string> public_id = std::nullopt);

    /// The session id, or nullopt for the per-user bucket.
    const std::optional<std::string>& id() const
    {
        return public_id_;
    }

    /// The value for @p key, or @p default_value when unset.
    Json get(const std::string& key, const Json& default_value = nullptr) const;
    void set(const std::string& key, const Json& value);
    void remove(const std::string& key);
    /// Empty the state; the session stays valid.
    void clear();
    /// Delete the session and all of its state.
    void end();

    /// Whether `create_session` recorded this session.
    bool exists() const;
    /// Record the session (what `create_session` does).
    void create();

  private:
    std::optional<Json> load_raw() const;
    void save_raw(const Json& raw);

    std::shared_ptr<SessionStateStore> store_;
    std::optional<std::string> public_id_;
    std::string key_;
};

/// Tool-schema property for an agent-supplied session id argument:
/// `{"type":"string","description":<the create-then-pass contract>}`.
Json session_id_property();
extern const char* const kSessionIdDescription;

/// The per-user session for the current principal. @throws SessionAuthError
Session user_session(const std::shared_ptr<SessionStateStore>& store);
/// The per-user session, or nullopt on an unauthenticated request.
std::optional<Session> optional_user_session(const std::shared_ptr<SessionStateStore>& store);
/// Mint an unguessable id (uuid4) and record the session under the current principal.
std::string create_session(const std::shared_ptr<SessionStateStore>& store);
/// Resolve @p session_id under the current principal. @throws InvalidSession
Session get_session(const std::shared_ptr<SessionStateStore>& store, const std::string& session_id);

/// Contributes the `create_session` and `end_session` tools. Register it
/// when a tool takes a session id; without it no id can be minted.
// PYTHON REFERENCE: fastmcp/server/sessions.py:403-433 (SessionProvider)
class SessionProvider : public providers::Provider
{
  public:
    using StoreGetter = std::function<std::shared_ptr<SessionStateStore>()>;
    explicit SessionProvider(StoreGetter store);
    std::vector<tools::Tool> list_tools() const override;

  private:
    StoreGetter store_;
};

} // namespace fastmcpp::server
