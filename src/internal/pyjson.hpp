#pragma once
// JSON decoding and value formatting with the semantics of the reference
// runtime: Python's json.loads / json.dumps (accepted syntax, error messages
// and positions), repr() of the decoded values, base64.urlsafe_b64decode,
// and the text of a pydantic ValidationError.

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::internal::pyjson
{

using OrderedJson = nlohmann::ordered_json;

/// json.JSONDecodeError. what() is "<msg>: line L column C (char N)".
class JSONDecodeError : public std::invalid_argument
{
  public:
    JSONDecodeError(const std::string& msg, std::size_t lineno, std::size_t colno, std::size_t pos);
};

/// UnicodeDecodeError, e.g. "'utf-8' codec can't decode byte 0xff in
/// position 0: invalid start byte". A ValueError, but not a JSONDecodeError.
class UnicodeDecodeError : public std::invalid_argument
{
  public:
    using std::invalid_argument::invalid_argument;
};

/// binascii.Error, e.g. "Incorrect padding". A ValueError.
class Base64Error : public std::invalid_argument
{
  public:
    using std::invalid_argument::invalid_argument;
};

/// RecursionError: nesting deeper than the reference interpreter decodes.
/// Not a ValueError.
class RecursionError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Deepest nesting the reference interpreter decodes.
inline constexpr std::size_t kPythonNestingLimit = 2997;
/// Nesting accepted by default: values are copied recursively elsewhere,
/// so documents from the network are held to a depth that is safe on any
/// thread's stack. Real documents stay far below it.
inline constexpr std::size_t kNestingLimit = 256;

/// json.loads(bytes): detects UTF-8 (with or without a BOM), UTF-16 and
/// UTF-32, decodes with 'surrogatepass', then parses. NaN and Infinity are
/// accepted; a repeated key keeps its first position and last value.
/// Integers beyond 64 bits are kept exactly as binary values with subtype
/// kBigIntSubtype holding the decimal digits. Lone surrogates become U+FFFD.
/// @throws JSONDecodeError for invalid JSON.
/// @throws UnicodeDecodeError when the bytes are not valid in the detected
///         encoding.
/// @throws std::invalid_argument for an integer with more than 4300 digits.
/// @throws RecursionError, with the reference's message, when nesting
///         exceeds @p max_depth.
OrderedJson loads(const std::string& bytes, std::size_t max_depth = kNestingLimit);

/// Binary subtype marking an integer too large for 64 bits.
inline constexpr std::uint8_t kBigIntSubtype = 0x49;

/// Whether @p value is an integer (including big integers; not bool).
bool is_int(const OrderedJson& value);

/// json.dumps with ensure_ascii. With @p indent, members go on their own
/// lines and separators are "," and ": "; without, ", " and ": ".
std::string dumps(const OrderedJson& value, std::optional<int> indent = std::nullopt);

/// base64.urlsafe_b64decode of a str: '-' and '_' stand for '+' and '/',
/// other characters outside the alphabet are skipped, and decoding stops at
/// the first complete padding.
/// @throws std::invalid_argument "string argument should contain only ASCII
///         characters" or Base64Error.
std::string urlsafe_b64decode(const std::string& text);

/// repr() of a decoded value: 'str', True, None, 1.5, [..], {'k': v}.
std::string repr(const OrderedJson& value);

/// repr() of a float.
std::string float_repr(double value);

/// type(value).__name__: dict, list, str, int, float, bool, NoneType.
std::string type_name(const OrderedJson& value);

/// str() of a decoded value (strings unquoted, everything else repr()).
std::string str(const OrderedJson& value);

/// Python truthiness: None, False, 0, "", [] and {} are false.
bool truthy(const OrderedJson& value);

/// @p text shortened as validation errors show input values: when longer
/// than 50 bytes, its first 25 and last 24 bytes (at character boundaries)
/// around "...".
std::string truncate_middle(const std::string& text);

/// One error of a pydantic ValidationError.
struct LineError
{
    /// Dotted location, e.g. "redirect_uris.1"; empty for the model itself.
    std::string loc;
    /// Error type, e.g. "missing", "string_type", "value_error".
    std::string type;
    std::string msg;
    OrderedJson input;
};

/// str() of a ValidationError for @p title (the model name), e.g.
/// "1 validation error for CIMDDocument\nredirect_uris\n  Field required
/// [type=missing, input_value={...}, input_type=dict]\n    For further
/// information visit https://errors.pydantic.dev/2.14/v/missing".
std::string validation_error_str(const std::string& title, const std::vector<LineError>& errors);

} // namespace fastmcpp::internal::pyjson
