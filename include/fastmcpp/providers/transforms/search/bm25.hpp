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

/// Tokenize text: lowercase, split on non-alphanumeric, filter short tokens.
inline std::vector<std::string> tokenize(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else
        {
            if (current.size() > 1)
                tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (current.size() > 1)
        tokens.push_back(std::move(current));
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
            indexed_tools_ = tools;
            last_hash_ = std::move(hash);
        }

        auto indices = index_.query(query, max_results());
        std::vector<tools::Tool> result;
        result.reserve(indices.size());
        for (int i : indices)
            result.push_back(indexed_tools_[i]);
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
        std::string combined;
        for (const auto& t : texts)
        {
            combined += t;
            combined += '|';
        }
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
    mutable std::vector<tools::Tool> indexed_tools_;
    mutable std::string last_hash_;
};

} // namespace fastmcpp::providers::transforms::search
