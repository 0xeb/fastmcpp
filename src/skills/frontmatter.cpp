// Strict SKILL.md frontmatter: a YAML subset parser and emitter.
//
// fastmcp parses frontmatter with PyYAML's SafeLoader (YAML 1.1 scalar
// resolution), rejects aliases and duplicate keys, and accepts only finite JSON
// values. This parser implements the subset of YAML that skill frontmatter uses
// with the same resolution rules, and rejects anything outside it rather than
// guessing, so a file this port accepts is one fastmcp accepts too.
//

#include "fastmcpp/skills/skill.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace fastmcpp::skills
{

namespace
{

struct YamlError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        --e;
    return s.substr(b, e - b);
}

std::size_t indent_of(const std::string& line)
{
    std::size_t n = 0;
    while (n < line.size() && line[n] == ' ')
        ++n;
    if (n < line.size() && line[n] == '\t')
        throw YamlError("found character '\\t' that cannot start any token");
    return n;
}

bool is_blank_or_comment(const std::string& line)
{
    auto t = trim(line);
    return t.empty() || t[0] == '#';
}

/// Strip a trailing ` #comment` from a plain scalar fragment.
std::string strip_comment(const std::string& text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t'))
            return text.substr(0, i);
    return text;
}

void append_utf8(std::string& out, unsigned long cp)
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

/// YAML 1.1 implicit resolution of a plain scalar, as PyYAML's SafeLoader does.
Json resolve_plain(const std::string& text)
{
    static const std::regex kNull("^(~|null|Null|NULL|)$");
    static const std::regex kBool(
        "^(yes|Yes|YES|no|No|NO|true|True|TRUE|false|False|FALSE|on|On|ON|off|Off|OFF)$");
    static const std::regex kInt(
        "^([-+]?0b[0-1_]+|[-+]?0[0-7_]+|[-+]?(0|[1-9][0-9_]*)|[-+]?0x[0-9a-fA-F_]+|"
        "[-+]?[1-9][0-9_]*(:[0-5]?[0-9])+)$");
    static const std::regex kFloat(
        "^([-+]?[0-9][0-9_]*\\.[0-9_]*([eE][-+][0-9]+)?|\\.[0-9][0-9_]*([eE][-+][0-9]+)?|"
        "[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+\\.[0-9_]*|[-+]?\\.(inf|Inf|INF)|\\.(nan|NaN|NAN))$");
    static const std::regex kTimestamp(
        "^([0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]|"
        "[0-9][0-9][0-9][0-9]-[0-9][0-9]?-[0-9][0-9]?"
        "(?:[Tt]|[ \\t]+)[0-9][0-9]?:[0-9][0-9]:[0-9][0-9](?:\\.[0-9]*)?"
        "(?:[ \\t]*(?:Z|[-+][0-9][0-9]?(?::[0-9][0-9])?))?)$");

    // PyYAML resolves these to the merge and value tags, which the strict
    // frontmatter loader cannot construct.
    if (text == "<<" || text == "=")
        throw YamlError("merge keys and value tags are not supported");
    if (std::regex_match(text, kNull))
        return nullptr;
    if (std::regex_match(text, kBool))
    {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
        return c == 'y' || c == 't' || (c == 'o' && text.size() == 2);
    }
    if (std::regex_match(text, kInt))
    {
        std::string digits;
        for (char c : text)
            if (c != '_')
                digits.push_back(c);
        int sign = 1;
        std::size_t pos = 0;
        if (digits[0] == '-' || digits[0] == '+')
        {
            sign = digits[0] == '-' ? -1 : 1;
            pos = 1;
        }
        std::string body = digits.substr(pos);
        long long value = 0;
        if (body == "0")
            value = 0;
        else if (body.compare(0, 2, "0b") == 0)
            value = std::stoll(body.substr(2), nullptr, 2);
        else if (body.compare(0, 2, "0x") == 0)
            value = std::stoll(body.substr(2), nullptr, 16);
        else if (body[0] == '0')
            value = std::stoll(body, nullptr, 8);
        else if (body.find(':') != std::string::npos)
        {
            std::stringstream parts(body);
            std::string part;
            while (std::getline(parts, part, ':'))
                value = value * 60 + std::stoll(part);
        }
        else
            value = std::stoll(body);
        return sign * value;
    }
    if (std::regex_match(text, kFloat))
    {
        std::string digits;
        for (char c : text)
            if (c != '_')
                digits.push_back(c);
        const std::string lower = [&]
        {
            std::string l = digits;
            for (auto& c : l)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return l;
        }();
        if (lower.find("inf") != std::string::npos || lower.find("nan") != std::string::npos)
            throw ValidationError("frontmatter must contain only finite JSON values");
        if (digits.find(':') != std::string::npos)
        {
            int sign = 1;
            std::size_t pos = 0;
            if (digits[0] == '-' || digits[0] == '+')
            {
                sign = digits[0] == '-' ? -1 : 1;
                pos = 1;
            }
            double value = 0;
            std::stringstream parts(digits.substr(pos));
            std::string part;
            while (std::getline(parts, part, ':'))
                value = value * 60 + std::stod(part);
            return sign * value;
        }
        return std::strtod(digits.c_str(), nullptr);
    }
    // A date or timestamp becomes a datetime, which is not a JSON value.
    if (std::regex_match(text, kTimestamp))
        throw ValidationError("frontmatter must contain only finite JSON values");
    return text;
}

class Parser
{
  public:
    explicit Parser(const std::string& text)
    {
        std::size_t start = 0;
        while (start <= text.size())
        {
            auto end = text.find('\n', start);
            std::string line =
                text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines_.push_back(std::move(line));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

    Json parse_document()
    {
        skip_insignificant();
        if (at_end())
            return nullptr;
        if (trim(lines_[pos_]) == "---")
            throw YamlError("multiple documents are not supported");
        Json value = parse_node(indent_of(lines_[pos_]));
        skip_insignificant();
        if (!at_end())
            throw YamlError("expected <document end>, but found another node");
        return value;
    }

  private:
    std::vector<std::string> lines_;
    std::size_t pos_{0};

    bool at_end() const
    {
        return pos_ >= lines_.size();
    }

    void skip_insignificant()
    {
        while (!at_end() && is_blank_or_comment(lines_[pos_]))
            ++pos_;
    }

    static bool is_sequence_entry(const std::string& content)
    {
        return content == "-" || content.compare(0, 2, "- ") == 0;
    }

    static void reject_indicator(const std::string& content)
    {
        if (content.empty())
            return;
        switch (content[0])
        {
        case '&':
        case '*':
            throw ValidationError("frontmatter must not use YAML aliases");
        case '!':
            throw YamlError("tags are not supported");
        case '@':
        case '`':
            throw YamlError("found character that cannot start any token");
        case '%':
            throw YamlError("directives are not supported");
        case '?':
            if (content.size() == 1 || content[1] == ' ')
                throw YamlError("complex mapping keys are not supported");
            break;
        default:
            break;
        }
    }

    /// Position of the `:` ending a mapping key in @p content, or npos.
    static std::size_t key_colon(const std::string& content)
    {
        if (content.empty())
            return std::string::npos;
        std::size_t i = 0;
        if (content[0] == '"' || content[0] == '\'')
        {
            const char q = content[0];
            for (i = 1; i < content.size(); ++i)
            {
                if (q == '"' && content[i] == '\\')
                {
                    ++i;
                    continue;
                }
                if (content[i] == q)
                {
                    if (q == '\'' && i + 1 < content.size() && content[i + 1] == '\'')
                    {
                        ++i;
                        continue;
                    }
                    break;
                }
            }
            ++i;
            std::size_t j = i;
            while (j < content.size() && content[j] == ' ')
                ++j;
            if (j < content.size() && content[j] == ':' &&
                (j + 1 == content.size() || content[j + 1] == ' '))
                return j;
            return std::string::npos;
        }
        for (i = 0; i < content.size(); ++i)
        {
            if (content[i] == '#' && i > 0 && content[i - 1] == ' ')
                return std::string::npos;
            if (content[i] == ':' && (i + 1 == content.size() || content[i + 1] == ' '))
                return i;
        }
        return std::string::npos;
    }

    Json parse_node(std::size_t indent)
    {
        skip_insignificant();
        if (at_end())
            return nullptr;
        const std::string& line = lines_[pos_];
        const std::size_t ind = indent_of(line);
        if (ind < indent)
            return nullptr;
        const std::string content = line.substr(ind);
        reject_indicator(content);
        if (is_sequence_entry(content))
            return parse_sequence(ind);
        if (key_colon(content) != std::string::npos)
            return parse_mapping(ind);
        // A bare scalar document or nested value.
        ++pos_;
        return parse_inline_value(trim(content), indent == 0 ? 0 : indent - 1);
    }

    Json parse_sequence(std::size_t ind)
    {
        Json result = Json::array();
        while (true)
        {
            skip_insignificant();
            if (at_end())
                break;
            std::string& line = lines_[pos_];
            const std::size_t li = indent_of(line);
            if (li != ind)
            {
                if (li > ind)
                    throw YamlError("expected <block end>, but found a more indented entry");
                break;
            }
            const std::string content = line.substr(ind);
            if (!is_sequence_entry(content))
                break;
            const std::string rest = content.size() > 1 ? content.substr(2) : std::string();
            if (trim(strip_comment(rest)).empty())
            {
                ++pos_;
                skip_insignificant();
                if (!at_end() && indent_of(lines_[pos_]) > ind)
                    result.push_back(parse_node(ind + 1));
                else
                    result.push_back(nullptr);
                continue;
            }
            // Re-read the entry's content at its own column, as a nested node.
            const std::size_t lead = rest.find_first_not_of(' ');
            const std::size_t inner = ind + 2 + lead;
            line = std::string(inner, ' ') + rest.substr(lead);
            result.push_back(parse_node(ind + 1));
        }
        return result;
    }

    Json parse_mapping(std::size_t ind)
    {
        Json result = Json::object();
        while (true)
        {
            skip_insignificant();
            if (at_end())
                break;
            const std::string line = lines_[pos_];
            const std::size_t li = indent_of(line);
            if (li != ind)
            {
                if (li > ind)
                    throw YamlError("mapping values are not allowed here");
                break;
            }
            const std::string content = line.substr(ind);
            if (is_sequence_entry(content))
                break;
            reject_indicator(content);
            const std::size_t colon = key_colon(content);
            if (colon == std::string::npos)
                throw YamlError("could not find expected ':'");
            const std::string key = parse_key(trim(content.substr(0, colon)));
            if (result.contains(key))
                throw ValidationError("duplicate frontmatter key: " + key);
            const std::string raw_rest = content.substr(colon + 1);
            const auto first = raw_rest.find_first_not_of(" \t");
            if (raw_rest.substr(0, first).find('\t') != std::string::npos &&
                first != std::string::npos)
                throw YamlError("found character '\\t' that cannot start any token");
            const std::string rest = trim(raw_rest);
            ++pos_;
            if (trim(strip_comment(rest)).empty())
            {
                skip_insignificant();
                if (!at_end())
                {
                    const std::string& next = lines_[pos_];
                    const std::size_t ni = indent_of(next);
                    if (ni > ind)
                    {
                        result[key] = parse_node(ind + 1);
                        continue;
                    }
                    // A block sequence may sit at its key's indentation.
                    if (ni == ind && is_sequence_entry(next.substr(ni)))
                    {
                        result[key] = parse_sequence(ind);
                        continue;
                    }
                }
                result[key] = nullptr;
                continue;
            }
            result[key] = parse_inline_value(rest, ind);
        }
        return result;
    }

    static std::string parse_key(const std::string& raw)
    {
        if (raw.empty())
            throw ValidationError("frontmatter mapping keys must be strings");
        if (raw[0] == '"' || raw[0] == '\'')
        {
            std::size_t pos = 0;
            return parse_quoted(raw, pos);
        }
        reject_indicator(raw);
        if (raw[0] == '[' || raw[0] == '{')
            throw ValidationError("frontmatter mapping keys must be strings");
        Json resolved = resolve_plain(raw);
        if (!resolved.is_string())
            throw ValidationError("frontmatter mapping keys must be strings");
        return resolved.get<std::string>();
    }

    /// A value starting on the key's line; may continue on deeper lines.
    Json parse_inline_value(const std::string& rest, std::size_t parent_indent)
    {
        reject_indicator(rest);
        const char c = rest[0];
        if (c == '|' || c == '>')
            return parse_block_scalar(rest, parent_indent);
        if (c == '[' || c == '{')
        {
            std::string text = rest;
            // Flow collections may span lines; gather until brackets balance.
            while (!flow_balanced(text))
            {
                if (at_end())
                    throw YamlError("while parsing a flow collection: unexpected end");
                text += " " + trim(lines_[pos_++]);
            }
            std::size_t p = 0;
            Json value = parse_flow(text, p);
            skip_spaces(text, p);
            if (p < text.size() && text[p] != '#')
                throw YamlError("unexpected content after a flow collection");
            return value;
        }
        if (c == '"' || c == '\'')
        {
            std::string text = rest;
            while (!quote_closed(text))
            {
                if (at_end())
                    throw YamlError(
                        "while scanning a quoted scalar: found unexpected end of stream");
                text += "\n" + lines_[pos_++];
            }
            std::size_t p = 0;
            std::string value = parse_quoted(text, p);
            skip_spaces(text, p);
            if (p < text.size() && text[p] != '#')
                throw YamlError("unexpected content after a quoted scalar");
            return value;
        }
        if (c == '-' && (rest.size() == 1 || rest[1] == ' '))
            throw YamlError("block sequence entries are not allowed in this context");
        // Plain scalar, possibly continued on more-indented lines.
        std::string text = trim(strip_comment(rest));
        bool comment_seen = strip_comment(rest).size() != rest.size();
        check_plain(text);
        std::string folded = text;
        std::size_t newlines = 0;
        while (!at_end() && !comment_seen)
        {
            const std::string& next = lines_[pos_];
            if (trim(next).empty())
            {
                ++newlines;
                ++pos_;
                continue;
            }
            const std::size_t ni = indent_of(next);
            if (ni <= parent_indent || trim(next)[0] == '#')
                break;
            std::string piece = strip_comment(next.substr(ni));
            comment_seen = piece.size() != next.size() - ni;
            piece = trim(piece);
            check_plain(piece);
            folded += newlines ? std::string(newlines, '\n') : std::string(" ");
            folded += piece;
            newlines = 0;
            ++pos_;
        }
        if (folded != text)
            return folded;
        return resolve_plain(text);
    }

    static void check_plain(const std::string& text)
    {
        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i] == ':' && (i + 1 == text.size() || text[i + 1] == ' '))
                throw YamlError("mapping values are not allowed here");
    }

    Json parse_block_scalar(const std::string& header, std::size_t parent_indent)
    {
        const bool literal = header[0] == '|';
        char chomp = 'c';
        std::size_t explicit_indent = 0;
        for (std::size_t i = 1; i < header.size(); ++i)
        {
            const char h = header[i];
            if (h == '-' || h == '+')
                chomp = h;
            else if (h >= '1' && h <= '9')
                explicit_indent = static_cast<std::size_t>(h - '0');
            else if (h == ' ' || h == '\t')
            {
                if (trim(header.substr(i)).empty() || trim(header.substr(i))[0] == '#')
                    break;
                throw YamlError("expected a comment or a line break after a block scalar header");
            }
            else
                throw YamlError("expected chomping or indentation indicators");
        }
        std::size_t block_indent = explicit_indent ? parent_indent + explicit_indent : 0;
        // The frontmatter body ends without a line break, so the last line of
        // the document is unterminated: it contributes no trailing newline.
        std::vector<std::string> body;
        bool last_terminated = false;
        while (!at_end())
        {
            const std::string& line = lines_[pos_];
            const bool terminated = pos_ + 1 < lines_.size();
            if (trim(line).empty())
            {
                if (terminated)
                    body.emplace_back();
                ++pos_;
                continue;
            }
            std::size_t li = 0;
            while (li < line.size() && line[li] == ' ')
                ++li;
            if (block_indent == 0)
            {
                if (li <= parent_indent)
                    break;
                block_indent = li;
            }
            if (li < block_indent)
                break;
            body.push_back(line.substr(block_indent));
            last_terminated = terminated;
            ++pos_;
        }
        // Trailing empty lines are subject to chomping.
        std::size_t trailing = 0;
        while (!body.empty() && body.back().empty())
        {
            body.pop_back();
            ++trailing;
        }
        if (body.empty())
            last_terminated = false;
        std::string out;
        if (literal)
        {
            for (std::size_t i = 0; i < body.size(); ++i)
                out += (i ? "\n" : "") + body[i];
        }
        else
        {
            bool prev_more_indented = false;
            for (std::size_t i = 0; i < body.size(); ++i)
            {
                const std::string& l = body[i];
                const bool more_indented = !l.empty() && (l[0] == ' ' || l[0] == '\t');
                if (i > 0)
                {
                    const std::string& prev = body[i - 1];
                    if (l.empty())
                        out += "\n";
                    else if (prev.empty())
                        out += (more_indented || prev_more_indented) ? "\n" : "";
                    else if (more_indented || prev_more_indented)
                        out += "\n";
                    else
                        out += " ";
                }
                out += l;
                if (!l.empty())
                    prev_more_indented = more_indented;
            }
        }
        if (last_terminated && chomp != '-')
            out += "\n";
        if (chomp == '+')
            out += std::string(trailing, '\n');
        return out;
    }

    static void skip_spaces(const std::string& text, std::size_t& p)
    {
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n'))
            ++p;
    }

    static bool quote_closed(const std::string& text)
    {
        std::size_t p = 0;
        try
        {
            parse_quoted(text, p);
            return true;
        }
        catch (const YamlError&)
        {
            return false;
        }
    }

    static bool flow_balanced(const std::string& text)
    {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (quote)
            {
                if (quote == '"' && c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '[' || c == '{')
                ++depth;
            else if (c == ']' || c == '}')
                --depth;
        }
        return depth <= 0 && quote == 0;
    }

    /// A quoted scalar starting at text[p]; leaves p after the closing quote.
    static std::string parse_quoted(const std::string& text, std::size_t& p)
    {
        const char q = text[p++];
        std::string raw;
        bool closed = false;
        std::string out;
        // Line folding: a line break inside quotes becomes a space; blank lines newlines.
        auto fold = [](const std::string& s)
        {
            std::string result;
            std::size_t i = 0;
            while (i < s.size())
            {
                if (s[i] == '\n')
                {
                    // Trim trailing whitespace already appended.
                    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
                        result.pop_back();
                    std::size_t breaks = 0;
                    while (i < s.size() && (s[i] == '\n' || s[i] == ' ' || s[i] == '\t'))
                    {
                        if (s[i] == '\n')
                            ++breaks;
                        ++i;
                    }
                    result += breaks > 1 ? std::string(breaks - 1, '\n') : std::string(" ");
                    continue;
                }
                result.push_back(s[i++]);
            }
            return result;
        };
        if (q == '\'')
        {
            while (p < text.size())
            {
                if (text[p] == '\'')
                {
                    if (p + 1 < text.size() && text[p + 1] == '\'')
                    {
                        raw.push_back('\'');
                        p += 2;
                        continue;
                    }
                    ++p;
                    closed = true;
                    break;
                }
                raw.push_back(text[p++]);
            }
            if (!closed)
                throw YamlError("while scanning a quoted scalar: found unexpected end of stream");
            return fold(raw);
        }
        // Double-quoted: fold first over unescaped text, then process escapes.
        std::string segment;
        while (p < text.size())
        {
            const char c = text[p];
            if (c == '"')
            {
                ++p;
                closed = true;
                break;
            }
            if (c == '\\')
            {
                if (p + 1 >= text.size())
                    break;
                out += fold(segment);
                segment.clear();
                const char e = text[p + 1];
                p += 2;
                auto hex = [&](std::size_t n)
                {
                    if (p + n > text.size())
                        throw YamlError("expected escape sequence of hexadecimal numbers");
                    const std::string digits = text.substr(p, n);
                    for (char d : digits)
                        if (!std::isxdigit(static_cast<unsigned char>(d)))
                            throw YamlError("expected escape sequence of hexadecimal numbers");
                    p += n;
                    append_utf8(out, std::stoul(digits, nullptr, 16));
                };
                switch (e)
                {
                case '0':
                    out.push_back('\0');
                    break;
                case 'a':
                    out.push_back('\a');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 't':
                case '\t':
                    out.push_back('\t');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'v':
                    out.push_back('\v');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'e':
                    out.push_back('\x1b');
                    break;
                case ' ':
                    out.push_back(' ');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case 'N':
                    append_utf8(out, 0x85);
                    break;
                case '_':
                    append_utf8(out, 0xA0);
                    break;
                case 'L':
                    append_utf8(out, 0x2028);
                    break;
                case 'P':
                    append_utf8(out, 0x2029);
                    break;
                case 'x':
                    hex(2);
                    break;
                case 'u':
                    hex(4);
                    break;
                case 'U':
                    hex(8);
                    break;
                case '\n':
                    // Escaped line break: join without a space.
                    while (p < text.size() && (text[p] == ' ' || text[p] == '\t'))
                        ++p;
                    break;
                default:
                    throw YamlError(std::string("found unknown escape character '") + e + "'");
                }
                continue;
            }
            segment.push_back(c);
            ++p;
        }
        if (!closed)
            throw YamlError("while scanning a quoted scalar: found unexpected end of stream");
        out += fold(segment);
        return out;
    }

    static Json parse_flow(const std::string& text, std::size_t& p)
    {
        skip_spaces(text, p);
        if (p >= text.size())
            throw YamlError("unexpected end of a flow collection");
        const char c = text[p];
        if (c == '[')
        {
            ++p;
            Json list = Json::array();
            while (true)
            {
                skip_spaces(text, p);
                if (p < text.size() && text[p] == ']')
                {
                    ++p;
                    return list;
                }
                if (p < text.size() && text[p] == ',')
                    throw YamlError("while parsing a flow node: expected the node content");
                list.push_back(parse_flow(text, p));
                skip_spaces(text, p);
                if (p < text.size() && text[p] == ',')
                {
                    ++p;
                    continue;
                }
                if (p < text.size() && text[p] == ']')
                {
                    ++p;
                    return list;
                }
                throw YamlError("while parsing a flow sequence: expected ',' or ']'");
            }
        }
        if (c == '{')
        {
            ++p;
            Json map = Json::object();
            while (true)
            {
                skip_spaces(text, p);
                if (p < text.size() && text[p] == '}')
                {
                    ++p;
                    return map;
                }
                Json key_value = parse_flow_scalar(text, p, true);
                if (!key_value.is_string())
                    throw ValidationError("frontmatter mapping keys must be strings");
                const std::string key = key_value.get<std::string>();
                if (map.contains(key))
                    throw ValidationError("duplicate frontmatter key: " + key);
                skip_spaces(text, p);
                if (p < text.size() && text[p] == ':')
                {
                    ++p;
                    skip_spaces(text, p);
                    if (p < text.size() && (text[p] == ',' || text[p] == '}'))
                        map[key] = nullptr;
                    else
                        map[key] = parse_flow(text, p);
                }
                else
                    map[key] = nullptr;
                skip_spaces(text, p);
                if (p < text.size() && text[p] == ',')
                {
                    ++p;
                    continue;
                }
                if (p < text.size() && text[p] == '}')
                {
                    ++p;
                    return map;
                }
                throw YamlError("while parsing a flow mapping: expected ',' or '}'");
            }
        }
        return parse_flow_scalar(text, p, false);
    }

    static Json parse_flow_scalar(const std::string& text, std::size_t& p, bool is_key)
    {
        skip_spaces(text, p);
        if (p < text.size() && (text[p] == '"' || text[p] == '\''))
            return parse_quoted(text, p);
        if (p < text.size() && (text[p] == '[' || text[p] == '{'))
        {
            if (is_key)
                throw ValidationError("frontmatter mapping keys must be strings");
            return parse_flow(text, p);
        }
        reject_indicator(text.substr(p));
        std::string raw;
        while (p < text.size())
        {
            const char c = text[p];
            if (c == ',' || c == ']' || c == '}' || c == '[' || c == '{')
                break;
            if (c == ':' && (p + 1 == text.size() || text[p + 1] == ' ' || text[p + 1] == ',' ||
                             text[p + 1] == '}' || text[p + 1] == ']'))
                break;
            if (c == '#' && (raw.empty() || raw.back() == ' '))
                throw YamlError("comments are not allowed inside a flow collection");
            raw.push_back(c);
            ++p;
        }
        return resolve_plain(trim(raw));
    }
};

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------

// Mirrors PyYAML's Emitter.analyze_scalar / choose_scalar_style for
// safe_dump(allow_unicode=True), so authored SKILL.md bytes match fastmcp's.

bool has_special_characters(const std::string& s)
{
    for (unsigned char c : s)
        if ((c < 0x20 && c != '\n') || c == 0x7F)
            return true;
    return false;
}

bool plain_safe(const std::string& s)
{
    if (s.empty() || s.front() == ' ' || s.back() == ' ')
        return false;
    if (s.find('\n') != std::string::npos || has_special_characters(s))
        return false;
    static const std::string kAlways = "#,[]{}&*!|>'\"%@`";
    if (kAlways.find(s[0]) != std::string::npos)
        return false;
    if ((s[0] == '-' || s[0] == '?' || s[0] == ':') && (s.size() == 1 || s[1] == ' '))
        return false;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == ':' && (i + 1 == s.size() || s[i + 1] == ' '))
            return false;
        if (s[i] == '#' && i > 0 && s[i - 1] == ' ')
            return false;
    }
    try
    {
        return resolve_plain(s).is_string();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

constexpr std::size_t kBestWidth = 80;

/// Write @p text, replacing a single space with a line break and @p indent
/// spaces once the line is past the best width, as PyYAML does.
void write_wrapped(std::string& out, std::size_t& column, const std::string& text,
                   std::size_t indent, bool quoted)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == ' ')
        {
            std::size_t end = i;
            while (end < text.size() && text[end] == ' ')
                ++end;
            const bool interior = !quoted || (i != 0 && end != text.size());
            if (end == i + 1 && column > kBestWidth && interior && end != text.size())
            {
                out += "\n" + std::string(indent, ' ');
                column = indent;
            }
            else
            {
                out.append(text, i, end - i);
                column += end - i;
            }
            i = end;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        out.push_back(text[i]);
        if ((c & 0xC0) != 0x80)
            ++column;
        ++i;
    }
}

void emit_scalar(std::string& out, std::size_t& column, const Json& v, std::size_t indent)
{
    auto put = [&](const std::string& text)
    {
        out += text;
        column += text.size();
    };
    if (v.is_null())
        return put("null");
    if (v.is_boolean())
        return put(v.get<bool>() ? "true" : "false");
    if (v.is_number_integer() || v.is_number_unsigned())
        return put(v.dump());
    if (v.is_number_float())
    {
        std::string text = v.dump();
        // PyYAML's represent_float: a dot before the exponent, a signed exponent.
        if (text.find('.') == std::string::npos)
        {
            auto e = text.find_first_of("eE");
            text.insert(e == std::string::npos ? text.size() : e, ".0");
        }
        auto e = text.find_first_of("eE");
        if (e != std::string::npos && e + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[e + 1])))
            text.insert(e + 1, "+");
        return put(text);
    }
    const std::string s = v.get<std::string>();
    if (plain_safe(s))
        return write_wrapped(out, column, s, indent, false);
    if (!has_special_characters(s) && s.find('\n') == std::string::npos)
    {
        std::string quoted;
        for (char c : s)
        {
            quoted.push_back(c);
            if (c == '\'')
                quoted.push_back('\'');
        }
        put("'");
        write_wrapped(out, column, quoted, indent, true);
        return put("'");
    }
    // Control characters and line breaks: a double-quoted scalar.
    put(Json(s).dump(-1, ' ', false, Json::error_handler_t::strict));
}

void emit(std::string& out, const Json& v, std::size_t indent)
{
    const std::string pad(indent, ' ');
    auto scalar_line = [&](const std::string& lead, const Json& value, std::size_t value_indent)
    {
        out += lead;
        std::size_t column = lead.size();
        if (value.is_object() || value.is_array())
            out += value.is_object() ? "{}" : "[]";
        else
            emit_scalar(out, column, value, value_indent);
        out += "\n";
    };
    if (v.is_object())
    {
        for (const auto& [key, value] : v.items())
        {
            std::string key_text;
            std::size_t key_column = 0;
            emit_scalar(key_text, key_column, Json(key), indent + 2);
            if ((value.is_object() || value.is_array()) && !value.empty())
            {
                out += pad + key_text + ":\n";
                // PyYAML does not indent a block sequence under a mapping key.
                emit(out, value, value.is_object() ? indent + 2 : indent);
            }
            else
                scalar_line(pad + key_text + ": ", value, indent + 2);
        }
        return;
    }
    for (const auto& item : v)
        if ((item.is_object() || item.is_array()) && !item.empty())
        {
            std::string nested;
            emit(nested, item, indent + 2);
            // "- " replaces the first two columns of the nested block.
            out += pad + "- " + nested.substr(indent + 2);
        }
        else
            scalar_line(pad + "- ", item, indent + 2);
}

} // namespace

Json parse_frontmatter(std::string_view content)
{
    std::string text(content);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    // \A---\r?\n(.*?)\r?\n---(?:\r?\n|\Z), non-greedy.
    std::size_t start = 0;
    if (text.compare(0, 4, "---\n") == 0)
        start = 4;
    else if (text.compare(0, 5, "---\r\n") == 0)
        start = 5;
    else
        throw ValidationError("SKILL.md must begin with YAML frontmatter");
    std::optional<std::size_t> body_end;
    for (std::size_t p = start; p < text.size(); ++p)
    {
        std::size_t q = p;
        if (text[q] == '\r')
            ++q;
        if (q >= text.size() || text[q] != '\n')
            continue;
        if (text.compare(q + 1, 3, "---") != 0)
            continue;
        const std::size_t after = q + 4;
        if (after == text.size() || text.compare(after, 1, "\n") == 0 ||
            text.compare(after, 2, "\r\n") == 0)
        {
            body_end = p;
            break;
        }
    }
    if (!body_end)
        throw ValidationError("SKILL.md must begin with YAML frontmatter");
    const std::string body = text.substr(start, *body_end - start);
    Json value;
    try
    {
        value = Parser(body).parse_document();
    }
    catch (const YamlError& e)
    {
        throw ValidationError(std::string("frontmatter is not valid YAML: ") + e.what());
    }
    catch (const std::out_of_range&)
    {
        throw ValidationError("frontmatter is not valid YAML: integer out of range");
    }
    if (!value.is_object())
        throw ValidationError("frontmatter must be a mapping");
    validate_frontmatter(value);
    return value;
}

std::string render_skill_md(const Json& frontmatter, const std::string& instructions)
{
    // nlohmann::json objects iterate in key order; the Agent Skills convention
    // leads with name and description.
    Json ordered_tail = frontmatter;
    std::string header;
    for (const char* key : {"name", "description"})
        if (frontmatter.contains(key))
        {
            emit(header, Json{{key, frontmatter[key]}}, 0);
            ordered_tail.erase(key);
        }
    emit(header, ordered_tail, 0);
    return "---\n" + header + "---\n" + instructions;
}

} // namespace fastmcpp::skills
