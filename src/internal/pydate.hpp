#pragma once
// HTTP date parsing with the semantics of the reference runtime's
// email.utils.parsedate_to_datetime, and its int() for header values.

#include <optional>
#include <string>

namespace fastmcpp::internal::pydate
{

/// int(text) for an ASCII header value: surrounding whitespace, an optional
/// sign, digits with single underscores between them. nullopt where the
/// reference raises ValueError, or when the value does not fit in 64 bits.
std::optional<long long> py_int(const std::string& text);

/// parsedate_to_datetime(text).timestamp(), with a date that names no zone
/// (or "-0000") taken as UTC. Accepts RFC 5322, RFC 850 and asctime forms.
/// nullopt where the reference raises ValueError.
std::optional<double> parsedate_timestamp(const std::string& text);

} // namespace fastmcpp::internal::pydate
