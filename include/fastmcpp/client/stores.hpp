#pragma once
/// @file client/stores.hpp
/// @brief Persistent KeyValueStore implementations compatible with the
///        py-key-value stores fastmcp uses: a directory tree of JSON files and
///        a Fernet-encrypting wrapper. Data written by either implementation
///        reads back in the other.

#include "fastmcpp/client/auth.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace fastmcpp::util
{
class Fernet;
}

namespace fastmcpp::client::auth
{

/// One JSON file per key: `<root>/<collection>/<key>.json`, plus a
/// `<collection>-info.json` per collection. Collection and key names outside
/// [A-Za-z0-9_] are sanitized as `S_<name>-<hash>`; names that already start
/// with `S_` or `H_` are stored as given. Entries carry ISO 8601
/// `created_at` / `expires_at` stamps; expired entries read as absent.
class FileTreeStore : public KeyValueStore
{
  public:
    /// Creates @p data_directory when missing.
    explicit FileTreeStore(const std::filesystem::path& data_directory);
    ~FileTreeStore() override;

    std::optional<Json> get(const std::string& collection, const std::string& key) override;
    void put(const std::string& collection, const std::string& key, const Json& value,
             std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    void remove(const std::string& collection, const std::string& key) override;
    /// True when this call deleted the file.
    bool erase(const std::string& collection, const std::string& key) override;

    /// Delete every expired entry under the data directory.
    void cull();

    const std::filesystem::path& data_directory() const;

    /// The file name stem a collection or key is stored under.
    std::string sanitize_collection(const std::string& collection) const;
    std::string sanitize_key(const std::string& key) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Encrypts each value with Fernet before it reaches the wrapped store, as
/// `{"__encrypted_data__": <base64>, "__encryption_version__": 1}`. Values that
/// are not in that form are returned unchanged.
class FernetEncryptionWrapper : public KeyValueStore
{
  public:
    /// @p fernet_key is 32 url-safe base64-encoded bytes. With
    /// @p raise_on_decryption_error false, a value that does not decrypt reads
    /// as absent.
    FernetEncryptionWrapper(std::shared_ptr<KeyValueStore> store, const std::string& fernet_key,
                            bool raise_on_decryption_error = true);
    ~FernetEncryptionWrapper() override;

    std::optional<Json> get(const std::string& collection, const std::string& key) override;
    void put(const std::string& collection, const std::string& key, const Json& value,
             std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    void remove(const std::string& collection, const std::string& key) override;
    bool erase(const std::string& collection, const std::string& key) override;

    const std::shared_ptr<KeyValueStore>& store() const
    {
        return store_;
    }

  private:
    std::shared_ptr<KeyValueStore> store_;
    std::unique_ptr<util::Fernet> fernet_;
    bool raise_on_decryption_error_;
};

/// A store value failed to decrypt.
class DecryptionError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

} // namespace fastmcpp::client::auth
