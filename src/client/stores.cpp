#include "fastmcpp/client/stores.hpp"

#include "../internal/pyjson.hpp"
#include "fastmcpp/util/crypto.hpp"
#include "fastmcpp/util/fernet.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fastmcpp::client::auth
{

namespace
{

namespace fs = std::filesystem;
namespace pyjson = fastmcpp::internal::pyjson;
using OJ = nlohmann::ordered_json;

const char* const kEncryptedDataKey = "__encrypted_data__";
const char* const kEncryptionVersionKey = "__encryption_version__";
constexpr int kEncryptionVersion = 1;

double epoch_now()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// datetime.isoformat() of a UTC time: microseconds only when non-zero.
std::string isoformat_utc(double epoch)
{
    auto secs = static_cast<std::int64_t>(std::floor(epoch));
    auto micros =
        static_cast<std::int64_t>(std::llround((epoch - static_cast<double>(secs)) * 1e6));
    if (micros >= 1000000)
    {
        secs += 1;
        micros -= 1000000;
    }
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string out = buf;
    if (micros)
    {
        std::snprintf(buf, sizeof buf, ".%06lld", static_cast<long long>(micros));
        out += buf;
    }
    return out + "+00:00";
}

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// datetime.fromisoformat of the forms this store and py-key-value write:
/// YYYY-MM-DD[T ]HH:MM:SS[.ffffff][+HH:MM|Z]. nullopt otherwise.
std::optional<double> parse_isoformat(const std::string& s)
{
    // The fixed-width decimal field at s[pos, pos + n), or -1.
    const auto field = [&s](std::size_t pos, std::size_t n)
    {
        if (pos + n > s.size())
            return -1;
        int v = 0;
        for (std::size_t k = pos; k < pos + n; ++k)
        {
            if (!std::isdigit(static_cast<unsigned char>(s[k])))
                return -1;
            v = v * 10 + (s[k] - '0');
        }
        return v;
    };
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':')
        return std::nullopt;
    const int y = field(0, 4), mo = field(5, 2), d = field(8, 2);
    const int h = field(11, 2), mi = field(14, 2), se = field(17, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0)
        return std::nullopt;
    std::size_t i = 19;
    double frac = 0;
    if (i < s.size() && s[i] == '.')
    {
        std::size_t j = i + 1;
        std::string digits;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
            digits.push_back(s[j++]);
        if (digits.empty())
            return std::nullopt;
        frac = std::stod("0." + digits);
        i = j;
    }
    int offset = 0;
    if (i < s.size())
    {
        if (s[i] == 'Z')
            ++i;
        else if (s[i] == '+' || s[i] == '-')
        {
            const int oh = field(i + 1, 2), om = field(i + 4, 2);
            if (oh < 0 || om < 0 || s[i + 3] != ':')
                return std::nullopt;
            offset = (oh * 3600 + om * 60) * (s[i] == '-' ? -1 : 1);
            i += 6;
        }
    }
    if (i != s.size())
        return std::nullopt;
    const std::int64_t days =
        days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return static_cast<double>(days * 86400 + h * 3600 + mi * 60 + se - offset) + frac;
}

OJ sorted_copy(const OJ& v)
{
    if (v.is_object())
    {
        std::vector<std::string> keys;
        for (auto it = v.begin(); it != v.end(); ++it)
            keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        OJ out = OJ::object();
        for (const auto& k : keys)
            out[k] = sorted_copy(v.at(k));
        return out;
    }
    if (v.is_array())
    {
        OJ out = OJ::array();
        for (const auto& e : v)
            out.push_back(sorted_copy(e));
        return out;
    }
    return v;
}

/// json.dumps(obj, sort_keys=True).
std::string dumps_sorted(const OJ& v)
{
    return pyjson::dumps(sorted_copy(v));
}

bool is_name_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string sha256_hex(const std::string& s)
{
    const auto d = util::crypto::sha256(s);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (auto b : d)
    {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

/// A UTF-8 string as code points, so lengths and truncation count
/// characters the way Python's str does.
std::u32string utf8_to_u32(const std::string& s)
{
    std::u32string out;
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + n > s.size())
            n = 1;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 31u) : n == 3 ? (c & 15u) : (c & 7u);
        for (std::size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        out.push_back(cp);
        i += n;
    }
    return out;
}

/// py-key-value HybridSanitizationStrategy over [A-Za-z0-9_], hash fragment
/// only when the name changed.
std::string hybrid_sanitize(const std::string& value, std::size_t max_length)
{
    const std::u32string chars = utf8_to_u32(value);
    std::string sanitized;
    bool last_replaced = false;
    bool any_changed = false;
    for (char32_t c : chars)
    {
        if (c < 128 && is_name_char(static_cast<char>(c)))
        {
            sanitized.push_back(static_cast<char>(c));
            last_replaced = false;
        }
        else
        {
            any_changed = true;
            if (last_replaced)
                continue;
            sanitized.push_back('_');
            last_replaced = true;
        }
    }
    if (sanitized.size() == 1 && last_replaced)
        sanitized.clear();
    const bool changed = any_changed;
    if (changed)
    {
        const std::string hash = sha256_hex(value).substr(0, 8);
        const std::size_t overhead = 2 + 1 + 8;
        const std::size_t max_value = max_length > overhead ? max_length - overhead : 0;
        if (sanitized.size() > max_value)
            sanitized.resize(max_value);
        return "S_" + sanitized + "-" + hash;
    }
    // Only ASCII name characters remain, so byte length equals character length.
    if (sanitized.size() > max_length)
    {
        sanitized.resize(max_length);
        std::string prefixed = "S_" + sanitized;
        if (prefixed.size() > max_length)
            prefixed = "S_" + sanitized.substr(0, max_length - 2);
        return prefixed;
    }
    return sanitized;
}

std::size_t max_file_name_length(const fs::path& root)
{
#ifdef _WIN32
    (void)root;
    return 255;
#else
    const long v = ::pathconf(root.string().c_str(), _PC_NAME_MAX);
    return v > 0 ? static_cast<std::size_t>(v) : 255;
#endif
}

std::size_t max_path_length(const fs::path& root)
{
#ifdef _WIN32
    (void)root;
    return 260;
#else
    const long v = ::pathconf(root.string().c_str(), _PC_PATH_MAX);
    return v > 0 ? static_cast<std::size_t>(v) : 260;
#endif
}

std::optional<std::string> read_text(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_atomic(const fs::path& file, const std::string& text)
{
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex rng_mu;
    std::uint64_t r;
    {
        std::lock_guard<std::mutex> lock(rng_mu);
        r = rng();
    }
    std::ostringstream name;
    name << "tmp" << std::hex << r << ".tmp";
    const fs::path tmp = file.parent_path() / name.str();
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
        out << text;
        out.flush();
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec)
    {
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
        if (ec)
        {
            fs::remove(tmp, ec);
            throw std::runtime_error("cannot replace " + file.string() + ": " + ec.message());
        }
    }
}

/// A parsed ManagedEntry document: value plus optional expiry.
struct Entry
{
    OJ value;
    std::optional<double> expires_at;
};

std::optional<Entry> parse_entry(const std::string& text)
{
    OJ doc;
    try
    {
        doc = pyjson::loads(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (!doc.is_object() || !doc.contains("value"))
        return std::nullopt;
    Entry e;
    const OJ& v = doc["value"];
    if (v.is_string())
    {
        try
        {
            e.value = pyjson::loads(v.get<std::string>());
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }
    else
        e.value = v;
    if (!e.value.is_object())
        return std::nullopt;
    if (doc.contains("expires_at") && doc["expires_at"].is_string())
        e.expires_at = parse_isoformat(doc["expires_at"].get<std::string>());
    return e;
}

bool is_expired(const Entry& e)
{
    return e.expires_at && *e.expires_at <= epoch_now();
}

} // namespace

struct FileTreeStore::Impl
{
    fs::path root;
    std::size_t collection_max = 245;
    std::size_t key_max = 250;
    std::mutex mu;
    std::map<std::string, fs::path> collections;

    fs::path collection_dir(const std::string& collection)
    {
        if (auto it = collections.find(collection); it != collections.end())
            return it->second;
        const std::string name = hybrid_sanitize(collection, collection_max);
        const fs::path dir = root / name;
        fs::create_directories(dir);
        const fs::path info = root / (name + "-info.json");
        if (!fs::exists(info))
        {
            OJ j = OJ::object();
            j["collection"] = collection;
            j["created_at"] = isoformat_utc(epoch_now());
            j["directory"] = fs::absolute(dir).string();
            j["version"] = 1;
            write_atomic(info, dumps_sorted(j));
        }
        collections[collection] = dir;
        return dir;
    }

    fs::path key_file(const std::string& collection, const std::string& key)
    {
        return collection_dir(collection) / (hybrid_sanitize(key, key_max) + ".json");
    }
};

FileTreeStore::FileTreeStore(const std::filesystem::path& data_directory)
    : impl_(std::make_unique<Impl>())
{
    fs::create_directories(data_directory);
    impl_->root = fs::weakly_canonical(fs::absolute(data_directory));
    const std::size_t name_max = max_file_name_length(impl_->root);
    impl_->collection_max = name_max - 10;
    const std::size_t path_max = max_path_length(impl_->root);
    const std::size_t used = impl_->root.generic_string().size();
    const std::size_t remaining = path_max > used ? path_max - used : 0;
    impl_->key_max = std::min(remaining, name_max - 5);
}

FileTreeStore::~FileTreeStore() = default;

const std::filesystem::path& FileTreeStore::data_directory() const
{
    return impl_->root;
}

std::string FileTreeStore::sanitize_collection(const std::string& collection) const
{
    return hybrid_sanitize(collection, impl_->collection_max);
}

std::string FileTreeStore::sanitize_key(const std::string& key) const
{
    return hybrid_sanitize(key, impl_->key_max);
}

std::optional<Json> FileTreeStore::get(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto text = read_text(impl_->key_file(collection, key));
    if (!text)
        return std::nullopt;
    const auto e = parse_entry(*text);
    if (!e || is_expired(*e))
        return std::nullopt;
    return Json::parse(e->value.dump());
}

void FileTreeStore::put(const std::string& collection, const std::string& key, const Json& value,
                        std::optional<std::chrono::seconds> ttl)
{
    if (!value.is_object())
        throw std::invalid_argument("FileTreeStore values must be JSON objects");
    std::lock_guard<std::mutex> lock(impl_->mu);
    const double created = epoch_now();
    OJ doc = OJ::object();
    doc["created_at"] = isoformat_utc(created);
    if (ttl && ttl->count() > 0)
        doc["expires_at"] = isoformat_utc(created + static_cast<double>(ttl->count()));
    doc["value"] = OJ::parse(value.dump());
    doc["version"] = 1;
    write_atomic(impl_->key_file(collection, key), dumps_sorted(doc));
}

void FileTreeStore::remove(const std::string& collection, const std::string& key)
{
    erase(collection, key);
}

bool FileTreeStore::erase(const std::string& collection, const std::string& key)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::error_code ec;
    return fs::remove(impl_->key_file(collection, key), ec);
}

void FileTreeStore::cull()
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(impl_->root, ec))
    {
        if (!dir.is_directory())
            continue;
        for (const auto& f : fs::directory_iterator(dir.path(), ec))
        {
            if (!f.is_regular_file() || f.path().extension() != ".json")
                continue;
            const auto text = read_text(f.path());
            if (!text)
                continue;
            const auto e = parse_entry(*text);
            if (e && is_expired(*e))
                fs::remove(f.path(), ec);
        }
    }
}

// ------------------------------------------------------------------ Fernet wrapper

FernetEncryptionWrapper::FernetEncryptionWrapper(std::shared_ptr<KeyValueStore> store,
                                                 const std::string& fernet_key,
                                                 bool raise_on_decryption_error)
    : store_(std::move(store)), fernet_(std::make_unique<util::Fernet>(fernet_key)),
      raise_on_decryption_error_(raise_on_decryption_error)
{
}

FernetEncryptionWrapper::~FernetEncryptionWrapper() = default;

std::optional<Json> FernetEncryptionWrapper::get(const std::string& collection,
                                                 const std::string& key)
{
    auto value = store_->get(collection, key);
    if (!value)
        return std::nullopt;
    if (!value->is_object() || !value->contains(kEncryptedDataKey))
        return value;
    const auto fail = [this](const std::string& msg) -> std::optional<Json>
    {
        if (raise_on_decryption_error_)
            throw DecryptionError(msg);
        return std::nullopt;
    };
    if (!value->contains(kEncryptionVersionKey))
        return fail("missing encryption version key");
    const Json& ver = (*value)[kEncryptionVersionKey];
    if (!ver.is_number_integer())
        return fail("expected encryption version to be an int");
    const Json& data = (*value)[kEncryptedDataKey];
    if (!data.is_string())
        return fail("expected encrypted data to be a str");
    try
    {
        if (ver.get<std::int64_t>() > kEncryptionVersion)
            throw std::runtime_error("unsupported encryption version");
        const std::string b64 = data.get<std::string>();
        for (char c : b64)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '='))
                throw std::runtime_error("invalid base64");
        std::string urlsafe = b64;
        for (auto& c : urlsafe)
            c = c == '+' ? '-' : c == '/' ? '_' : c;
        const std::string token = pyjson::urlsafe_b64decode(urlsafe);
        const std::string plaintext = fernet_->decrypt(token);
        return Json::parse(pyjson::loads(plaintext).dump());
    }
    catch (const std::exception&)
    {
        return fail("Failed to decrypt value");
    }
}

void FernetEncryptionWrapper::put(const std::string& collection, const std::string& key,
                                  const Json& value, std::optional<std::chrono::seconds> ttl)
{
    const std::string token = fernet_->encrypt(pyjson::dumps_compact(OJ::parse(value.dump())));
    Json wrapped = {
        {kEncryptedDataKey, util::crypto::base64_encode(
                                reinterpret_cast<const std::uint8_t*>(token.data()), token.size())},
        {kEncryptionVersionKey, kEncryptionVersion}};
    store_->put(collection, key, wrapped, ttl);
}

void FernetEncryptionWrapper::remove(const std::string& collection, const std::string& key)
{
    store_->remove(collection, key);
}

bool FernetEncryptionWrapper::erase(const std::string& collection, const std::string& key)
{
    return store_->erase(collection, key);
}

} // namespace fastmcpp::client::auth
