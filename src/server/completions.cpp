#include "fastmcpp/server/completions.hpp"

namespace fastmcpp::server
{

Json completion_to_json(const std::optional<Completion>& completion)
{
    Json out = {{"values", Json::array()}};
    if (!completion)
        return out;
    const auto& values = completion->values;
    if (values.size() > kMaxCompletionValues)
    {
        out["values"] =
            std::vector<std::string>(values.begin(), values.begin() + kMaxCompletionValues);
        out["total"] = completion->total ? *completion->total : static_cast<int>(values.size());
        out["hasMore"] = true;
        return out;
    }
    out["values"] = values;
    if (completion->total)
        out["total"] = *completion->total;
    if (completion->has_more)
        out["hasMore"] = *completion->has_more;
    return out;
}

} // namespace fastmcpp::server
