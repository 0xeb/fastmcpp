#pragma once
/// @file versions.hpp
/// @brief Component-version comparison and deduplication helpers.
///
/// Parity with Python fastmcp `src/fastmcp/utilities/versions.py` (commit
/// 03673d9f). Versions compare token-by-token: numeric tokens compare
/// numerically (so "10" > "2"), non-numeric tokens fall back to
/// lexicographic comparison. Unversioned items (std::nullopt) sort
/// strictly lowest.

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmcpp::util::versions
{

namespace detail
{
inline bool is_digits(const std::string& s)
{
    return !s.empty() &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

inline std::string strip_leading_zeros(const std::string& s)
{
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0')
        ++i;
    return s.substr(i);
}

inline std::vector<std::string> split(const std::string& v)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : v)
    {
        if (c == '.' || c == '-' || c == '_')
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

inline int compare_token(const std::string& a, const std::string& b)
{
    if (a == b)
        return 0;
    if (is_digits(a) && is_digits(b))
    {
        const auto an = strip_leading_zeros(a);
        const auto bn = strip_leading_zeros(b);
        if (an.size() != bn.size())
            return an.size() < bn.size() ? -1 : 1;
        return an < bn ? -1 : 1;
    }
    return a < b ? -1 : 1;
}
} // namespace detail

/// Strip a leading "v" prefix (e.g. "v1.2" -> "1.2") to match the Python
/// reference's `lstrip("v")` behavior in VersionKey.__init__.
inline std::string normalize_version(std::string v)
{
    if (!v.empty() && v.front() == 'v')
        v.erase(0, 1);
    return v;
}

/// Compare two version strings.
/// Returns -1 if a < b, 0 if equal, 1 if a > b. None (nullopt) sorts strictly
/// below any concrete version.
inline int compare(const std::optional<std::string>& a, const std::optional<std::string>& b)
{
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    const auto an = normalize_version(*a);
    const auto bn = normalize_version(*b);
    const auto ap = detail::split(an);
    const auto bp = detail::split(bn);
    const size_t n = std::max(ap.size(), bp.size());
    for (size_t i = 0; i < n; ++i)
    {
        const std::string& at = i < ap.size() ? ap[i] : std::string("0");
        const std::string& bt = i < bp.size() ? bp[i] : std::string("0");
        const int cmp = detail::compare_token(at, bt);
        if (cmp != 0)
            return cmp;
    }
    return 0;
}

/// Deduplicated entry: the winning component plus the sorted list of
/// available versions that mapped to its key (descending).
template <typename T> struct DedupedEntry
{
    T item;
    std::vector<std::string> available_versions;
};

/// Group `items` by `key_fn(item)`, keep only the highest-version entry per
/// group, and report the list of available concrete versions for each group
/// (descending). Mirrors Python `dedupe_with_versions` from commit 03673d9f.
template <typename T, typename KeyFn, typename VersionFn>
std::vector<DedupedEntry<T>>
dedupe_with_versions(const std::vector<T>& items, KeyFn key_fn, VersionFn version_fn)
{
    // Preserve first-seen order of keys for stable output.
    std::vector<std::string> key_order;
    std::unordered_map<std::string, std::vector<size_t>> by_key;
    by_key.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i)
    {
        std::string k = key_fn(items[i]);
        auto [it, inserted] = by_key.emplace(k, std::vector<size_t>{});
        if (inserted)
            key_order.push_back(k);
        it->second.push_back(i);
    }

    std::vector<DedupedEntry<T>> result;
    result.reserve(by_key.size());
    for (const auto& key : key_order)
    {
        const auto& indices = by_key.at(key);
        size_t winner_idx = indices.front();
        for (size_t i : indices)
        {
            if (compare(version_fn(items[i]), version_fn(items[winner_idx])) > 0)
                winner_idx = i;
        }

        std::vector<std::string> versions;
        versions.reserve(indices.size());
        for (size_t i : indices)
        {
            auto v = version_fn(items[i]);
            if (v)
                versions.push_back(*v);
        }
        if (!versions.empty())
        {
            std::sort(versions.begin(), versions.end(),
                      [](const std::string& a, const std::string& b)
                      {
                          return compare(std::optional<std::string>(a),
                                         std::optional<std::string>(b)) > 0;
                      });
        }

        result.push_back(DedupedEntry<T>{items[winner_idx], std::move(versions)});
    }
    return result;
}

} // namespace fastmcpp::util::versions
