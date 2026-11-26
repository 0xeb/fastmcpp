#pragma once

#include "fastmcpp/providers/transforms/search/base.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fastmcpp::providers::transforms::search
{

namespace detail
{

/// Decode one UTF-8 code point at @p i (advancing it); invalid bytes read as U+FFFD.
inline char32_t next_code_point(const std::string& s, size_t& i)
{
    const auto b0 = static_cast<unsigned char>(s[i++]);
    if (b0 < 0x80)
        return b0;
    int extra = b0 >= 0xF0 ? 3 : b0 >= 0xE0 ? 2 : b0 >= 0xC0 ? 1 : -1;
    if (extra < 0)
        return 0xFFFD;
    char32_t cp = b0 & (0x3F >> extra);
    for (int k = 0; k < extra; ++k)
    {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

inline void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Word characters: ASCII letters/digits, and every non-ASCII code point
/// except Latin-1 punctuation/symbols, general punctuation, spaces and
/// CJK/full-width punctuation (an approximation of Python's `[^\W_]`).
inline bool is_word_code_point(char32_t cp)
{
    if (cp < 0x80)
        return std::isalnum(static_cast<unsigned char>(cp)) != 0;
    if (cp <= 0xBF)
        return cp == 0xAA || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 || cp == 0xBA;
    if (cp == 0xD7 || cp == 0xF7 || cp == 0xFFFD)
        return false;
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
        (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
        (cp >= 0xFF5B && cp <= 0xFF65))
        return false;
    return true;
}

/// Lowercase ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic; full-width
/// ASCII letters/digits fold to ASCII (the common NFKC cases).
inline char32_t fold_code_point(char32_t cp)
{
    if (cp >= 0xFF10 && cp <= 0xFF5A) // full-width digits and letters
        cp -= 0xFEE0;
    if (cp < 0x80)
        return static_cast<char32_t>(std::tolower(static_cast<int>(cp)));
    if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7))
        return cp + 0x20;
    if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 &&
        cp != 0x17F)
    {
        const bool odd_pairs = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        if (odd_pairs ? (cp % 2 == 1) : (cp % 2 == 0))
            return cp + 1;
        return cp;
    }
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2)
        return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F)
        return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F)
        return cp + 0x50;
    return cp;
}

/// Tokenize text into case-folded Unicode word tokens of two or more code points.
// PYTHON REFERENCE: fastmcp/server/transforms/search/bm25.py (_tokenize: NFKC + casefold,
// `[^\W_]{2,}`). Normalization is approximated: no decomposition tables here.
inline std::vector<std::string> tokenize(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    size_t length = 0;
    auto flush = [&]
    {
        if (length > 1)
            tokens.push_back(std::move(current));
        current.clear();
        length = 0;
    };
    for (size_t i = 0; i < text.size();)
    {
        const char32_t cp = fold_code_point(next_code_point(text, i));
        if (is_word_code_point(cp))
        {
            append_utf8(current, cp);
            ++length;
        }
        else
        {
            flush();
        }
    }
    flush();
    return tokens;
}

/// Self-contained BM25 Okapi index.
class BM25Index
{
  public:
    explicit BM25Index(double k1 = 1.5, double b = 0.75) : k1_(k1), b_(b) {}

    void build(const std::vector<std::string>& documents)
    {
        n_ = static_cast<int>(documents.size());
        doc_tokens_.clear();
        doc_lengths_.clear();
        tf_.clear();
        df_.clear();

        double total_length = 0;
        for (const auto& doc : documents)
        {
            auto tokens = tokenize(doc);
            doc_lengths_.push_back(static_cast<int>(tokens.size()));
            total_length += tokens.size();

            std::unordered_map<std::string, int> tf;
            std::unordered_set<std::string> seen;
            for (const auto& token : tokens)
            {
                tf[token]++;
                if (seen.insert(token).second)
                    df_[token]++;
            }
            tf_.push_back(std::move(tf));
            doc_tokens_.push_back(std::move(tokens));
        }
        avg_dl_ = n_ > 0 ? total_length / n_ : 0.0;
    }

    /// Return indices of top_k documents sorted by BM25 score.
    std::vector<int> query(const std::string& text, int top_k) const
    {
        auto query_tokens = tokenize(text);
        if (query_tokens.empty() || n_ == 0)
            return {};

        std::vector<double> scores(n_, 0.0);
        for (const auto& token : query_tokens)
        {
            auto df_it = df_.find(token);
            if (df_it == df_.end())
                continue;
            double idf = std::log((n_ - df_it->second + 0.5) / (df_it->second + 0.5) + 1.0);
            for (int i = 0; i < n_; ++i)
            {
                auto tf_it = tf_[i].find(token);
                if (tf_it == tf_[i].end())
                    continue;
                double tf = tf_it->second;
                double dl = doc_lengths_[i];
                double numerator = tf * (k1_ + 1);
                double denominator = tf + k1_ * (1 - b_ + b_ * dl / avg_dl_);
                scores[i] += idf * numerator / denominator;
            }
        }

        std::vector<int> indices(n_);
        std::iota(indices.begin(), indices.end(), 0);
        std::partial_sort(indices.begin(), indices.begin() + std::min(top_k, n_), indices.end(),
                          [&](int a, int b_idx) { return scores[a] > scores[b_idx]; });

        std::vector<int> result;
        for (int i = 0; i < std::min(top_k, n_); ++i)
            if (scores[indices[i]] > 0)
                result.push_back(indices[i]);
        return result;
    }

    double k1() const
    {
        return k1_;
    }
    double b() const
    {
        return b_;
    }

  private:
    double k1_;
    double b_;
    std::vector<std::vector<std::string>> doc_tokens_;
    std::vector<int> doc_lengths_;
    double avg_dl_ = 0.0;
    std::unordered_map<std::string, int> df_;
    std::vector<std::unordered_map<std::string, int>> tf_;
    int n_ = 0;
};

} // namespace detail

/// Search transform using BM25 Okapi relevance ranking.
///
/// Maintains an in-memory index that is lazily rebuilt when the tool
/// catalog changes (detected via hash of tool searchable text).
///
/// Parity with Python fastmcp BM25SearchTransform (commit c96c0400).
class BM25SearchTransform : public BaseSearchTransform
{
  public:
    explicit BM25SearchTransform(Options opts = {}) : BaseSearchTransform(std::move(opts)) {}

    std::vector<tools::Tool> do_search(const std::vector<tools::Tool>& tools,
                                       const std::string& query) const override
    {
        // Rebuild index if catalog changed
        auto hash = catalog_hash(tools);
        if (hash != last_hash_)
        {
            std::vector<std::string> documents;
            documents.reserve(tools.size());
            for (const auto& t : tools)
                documents.push_back(extract_searchable_text(t));
            index_ = detail::BM25Index(index_.k1(), index_.b());
            index_.build(documents);
            indexed_names_.clear();
            for (const auto& t : tools)
                indexed_names_.push_back(t.name());
            last_hash_ = std::move(hash);
        }

        // Results come from the current catalog, so a schema change that leaves the
        // searchable text alone is still served fresh.
        // PYTHON REFERENCE: fastmcp commit 32f981cac
        std::unordered_map<std::string, const tools::Tool*> by_name;
        for (const auto& t : tools)
            by_name.emplace(t.name(), &t);
        auto indices = index_.query(query, max_results());
        std::vector<tools::Tool> result;
        result.reserve(indices.size());
        for (int i : indices)
            if (auto it = by_name.find(indexed_names_[i]); it != by_name.end())
                result.push_back(*it->second);
        return result;
    }

  protected:
    tools::Tool make_search_tool() const override
    {
        Json input_schema = {
            {"type", "object"},
            {"properties",
             Json{{"query", Json{{"type", "string"},
                                 {"description", "Natural language query to search for tools"}}}}},
            {"required", Json::array({"query"})}};

        tools::Tool::Fn fn = [](const Json& /*args*/) -> Json
        {
            return Json{
                {"content", Json::array({Json{{"type", "text"},
                                              {"text", "Search tool: use with query argument"}}})}};
        };

        return tools::Tool(search_tool_name(), std::move(input_schema), Json::object(),
                           std::move(fn));
    }

  private:
    static std::string catalog_hash(const std::vector<tools::Tool>& tools)
    {
        // Simple hash based on sorted tool names and descriptions
        std::vector<std::string> texts;
        texts.reserve(tools.size());
        for (const auto& t : tools)
            texts.push_back(extract_searchable_text(t));
        std::sort(texts.begin(), texts.end());
        // JSON-encoded so ["a|b","c"] and ["a","b|c"] differ.
        const std::string combined = Json(texts).dump();
        // Simple hash — FNV-1a
        uint64_t hash = 14695981039346656037ULL;
        for (char c : combined)
        {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
        return std::to_string(hash);
    }

    mutable detail::BM25Index index_;
    mutable std::vector<std::string> indexed_names_;
    mutable std::string last_hash_;
};

} // namespace fastmcpp::providers::transforms::search
