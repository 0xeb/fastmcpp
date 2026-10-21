#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fastmcpp::util
{

/// A linear-time regular expression matcher (Pike VM) with Rust `regex` syntax.
///
/// Matching time is O(text length x program size) for every pattern, so there is no
/// catastrophic backtracking, and a search can be cut off at a deadline. It answers only
/// "is there a match anywhere in the text" (Rust's `Regex::is_match`), which is what
/// fastmcp's regex tool search needs.
///
/// Supported, as in Rust: literals, `.`, classes (`[a-z]`, `[^...]`, nested classes, `&&`,
/// `--`, `~~`, `[[:alpha:]]`), `\d \w \s` and negations, `\b \B \< \> \b{start} \b{end}`,
/// `^ $ \A \z`, alternation, groups `(...)`, `(?:...)`, `(?P<n>...)`, `(?<n>...)`, flags
/// `i m s U x u R` in `(?flags)` and `(?flags:...)` with `-` negation, greedy and lazy
/// `* + ? {n} {n,} {n,m}`, and escapes `\n \t \r \f \v \a \xHH \x{...} \uHHHH \u{...}
/// \UHHHHHHHH \U{...}`.
///
/// Rejected with an error, as Rust does: lookaround, backreferences, `\Z`, octal escapes,
/// unknown escapes and flags, unbalanced groups or classes, and programs over the size limit.
///
/// Divergences from Rust: `\p{..}` / `\P{..}` Unicode properties are rejected, and `\d \w \s`,
/// word boundaries and case-insensitive matching use ASCII rules (non-ASCII text still matches
/// literally and through `.` and classes).
class LinearRegex
{
  public:
    struct Error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    enum class Result
    {
        NoMatch,
        Match,
        Timeout,
    };

    /// Maximum compiled program size (instructions); larger patterns are rejected.
    static constexpr std::size_t kMaxProgramSize = 500000;

    /// Compile @p pattern (UTF-8). Throws LinearRegex::Error for invalid or unsupported syntax.
    static LinearRegex compile(std::string_view pattern);

    /// Search @p text (UTF-8) for a match anywhere; gives up with Result::Timeout once
    /// @p deadline has passed.
    Result search(std::string_view text, std::chrono::steady_clock::time_point deadline) const;

    /// Search without a deadline.
    bool search(std::string_view text) const;

    std::size_t program_size() const;

    struct Program;

  private:
    explicit LinearRegex(std::shared_ptr<const Program> program) : program_(std::move(program)) {}
    std::shared_ptr<const Program> program_;
};

} // namespace fastmcpp::util
