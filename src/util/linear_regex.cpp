// Linear-time regex matcher with Rust `regex` syntax. See linear_regex.hpp.
//
// Parity target: fastmcp's RegexSearchTransform matches with Pydantic's Rust regex engine
// (fastmcp_slim/fastmcp/server/transforms/search/regex.py:57), which never backtracks.

#include "fastmcpp/util/linear_regex.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

namespace fastmcpp::util
{
namespace
{

constexpr char32_t kMaxCodepoint = 0x10FFFF;
constexpr int kMaxNesting = 250; // Rust regex-syntax's default nest limit

// ---------------------------------------------------------------- UTF-8

std::vector<char32_t> decode_utf8(std::string_view s)
{
    std::vector<char32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
    auto cont = [&](size_t k) { return i + k < s.size() && (byte(k) & 0xC0) == 0x80; };
    while (i < s.size())
    {
        const unsigned char b0 = byte(0);
        char32_t cp = 0xFFFD;
        size_t len = 1;
        if (b0 < 0x80)
        {
            cp = b0;
        }
        else if ((b0 & 0xE0) == 0xC0 && cont(1))
        {
            cp = (static_cast<char32_t>(b0 & 0x1F) << 6) | (byte(1) & 0x3F);
            if (cp >= 0x80)
                len = 2;
            else
                cp = 0xFFFD;
        }
        else if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2))
        {
            cp = (static_cast<char32_t>(b0 & 0x0F) << 12) |
                 (static_cast<char32_t>(byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
            len = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
                cp = 0xFFFD;
        }
        else if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3))
        {
            cp = (static_cast<char32_t>(b0 & 0x07) << 18) |
                 (static_cast<char32_t>(byte(1) & 0x3F) << 12) |
                 (static_cast<char32_t>(byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
            len = 4;
            if (cp < 0x10000 || cp > kMaxCodepoint)
                cp = 0xFFFD;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// ---------------------------------------------------------------- character sets

using Range = std::pair<char32_t, char32_t>;

struct CharSet
{
    std::vector<Range> ranges; // sorted and merged after normalize()

    void add(char32_t lo, char32_t hi)
    {
        ranges.emplace_back(lo, hi);
    }

    void normalize()
    {
        std::sort(ranges.begin(), ranges.end());
        std::vector<Range> merged;
        for (const auto& r : ranges)
            if (!merged.empty() && r.first <= merged.back().second + 1)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        ranges = std::move(merged);
    }

    CharSet negated() const
    {
        CharSet out;
        char32_t next = 0;
        for (const auto& r : ranges)
        {
            if (r.first > next)
                out.add(next, r.first - 1);
            next = r.second + 1;
        }
        if (next <= kMaxCodepoint)
            out.add(next, kMaxCodepoint);
        return out;
    }

    void unite(const CharSet& other)
    {
        ranges.insert(ranges.end(), other.ranges.begin(), other.ranges.end());
        normalize();
    }

    CharSet intersect(const CharSet& other) const
    {
        CharSet out;
        size_t a = 0;
        size_t b = 0;
        while (a < ranges.size() && b < other.ranges.size())
        {
            const char32_t lo = std::max(ranges[a].first, other.ranges[b].first);
            const char32_t hi = std::min(ranges[a].second, other.ranges[b].second);
            if (lo <= hi)
                out.add(lo, hi);
            if (ranges[a].second < other.ranges[b].second)
                ++a;
            else
                ++b;
        }
        return out;
    }

    CharSet minus(const CharSet& other) const
    {
        return intersect(other.negated());
    }

    CharSet symmetric_difference(const CharSet& other) const
    {
        CharSet u = *this;
        u.unite(other);
        return u.minus(intersect(other));
    }

    // ASCII case folding (see the divergence note in the header).
    void fold_case()
    {
        std::vector<Range> extra;
        for (const auto& r : ranges)
        {
            const char32_t ulo = std::max<char32_t>(r.first, 'A');
            const char32_t uhi = std::min<char32_t>(r.second, 'Z');
            if (ulo <= uhi)
                extra.emplace_back(ulo + 32, uhi + 32);
            const char32_t llo = std::max<char32_t>(r.first, 'a');
            const char32_t lhi = std::min<char32_t>(r.second, 'z');
            if (llo <= lhi)
                extra.emplace_back(llo - 32, lhi - 32);
        }
        ranges.insert(ranges.end(), extra.begin(), extra.end());
        normalize();
    }

    bool contains(char32_t c) const
    {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), c,
                                   [](char32_t v, const Range& r) { return v < r.first; });
        return it != ranges.begin() && c <= std::prev(it)->second;
    }
};

CharSet set_of(std::initializer_list<Range> rs)
{
    CharSet s;
    for (const auto& r : rs)
        s.add(r.first, r.second);
    s.normalize();
    return s;
}

CharSet digit_set()
{
    return set_of({{'0', '9'}});
}
CharSet word_set()
{
    return set_of({{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}});
}
CharSet space_set()
{
    return set_of({{'\t', '\r'}, {' ', ' '}});
}

bool is_word(char32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// ---------------------------------------------------------------- AST

enum class AssertKind
{
    StartText,
    EndText,
    StartLine,
    EndLine,
    WordBoundary,
    NotWordBoundary,
    WordStart,
    WordEnd,
};

struct Node
{
    enum class Kind
    {
        Empty,
        Set,
        Assert,
        Concat,
        Alt,
        Repeat,
    };
    static constexpr uint32_t kUnbounded = UINT32_MAX;

    Kind kind = Kind::Empty;
    CharSet set;
    AssertKind assert_kind = AssertKind::StartText;
    std::vector<Node> children;
    uint32_t min = 0;
    uint32_t max = 0;
    int height = 0;
};

Node make_set(CharSet s)
{
    Node n;
    n.kind = Node::Kind::Set;
    n.set = std::move(s);
    return n;
}

Node make_assert(AssertKind k)
{
    Node n;
    n.kind = Node::Kind::Assert;
    n.assert_kind = k;
    return n;
}

struct Flags
{
    bool i = false; // case-insensitive
    bool m = false; // multi-line ^ $
    bool s = false; // . matches \n
    bool x = false; // verbose (whitespace and # comments ignored)
};

[[noreturn]] void fail(const char* what)
{
    throw LinearRegex::Error(what);
}

// ---------------------------------------------------------------- parser

class Parser
{
  public:
    explicit Parser(std::vector<char32_t> p) : p_(std::move(p)) {}

    Node parse()
    {
        Flags flags;
        Node n = parse_alt(flags, 0);
        if (pos_ < p_.size())
            fail("unopened group");
        return n;
    }

  private:
    bool at_end() const
    {
        return pos_ >= p_.size();
    }
    char32_t peek(size_t k = 0) const
    {
        return pos_ + k < p_.size() ? p_[pos_ + k] : 0;
    }
    bool peek_is(char32_t c, size_t k = 0) const
    {
        return pos_ + k < p_.size() && p_[pos_ + k] == c;
    }
    char32_t next()
    {
        if (at_end())
            fail("unexpected end of pattern");
        return p_[pos_++];
    }

    void skip_verbose(const Flags& f)
    {
        if (!f.x)
            return;
        while (!at_end())
        {
            const char32_t c = peek();
            if (c == ' ' || (c >= '\t' && c <= '\r'))
                ++pos_;
            else if (c == '#')
                while (!at_end() && peek() != '\n')
                    ++pos_;
            else
                break;
        }
    }

    Node parse_alt(Flags& flags, int depth)
    {
        if (depth > kMaxNesting)
            fail("nesting limit exceeded");
        std::vector<Node> branches;
        branches.push_back(parse_concat(flags, depth));
        while (peek_is('|'))
        {
            ++pos_;
            branches.push_back(parse_concat(flags, depth));
        }
        if (branches.size() == 1)
            return std::move(branches[0]);
        Node n;
        n.kind = Node::Kind::Alt;
        n.children = std::move(branches);
        for (const auto& child : n.children)
            n.height = std::max(n.height, child.height + 1);
        if (n.height > kMaxNesting)
            fail("nesting limit exceeded");
        return n;
    }

    Node parse_concat(Flags& flags, int depth)
    {
        Node seq;
        seq.kind = Node::Kind::Concat;
        while (true)
        {
            skip_verbose(flags);
            if (at_end() || peek_is('|') || peek_is(')'))
                break;
            bool flags_only = false;
            Node atom = parse_atom(flags, depth, flags_only);
            if (flags_only)
                continue;
            seq.children.push_back(parse_quantifiers(std::move(atom), flags));
            seq.height = std::max(seq.height, seq.children.back().height + 1);
            if (seq.height > kMaxNesting)
                fail("nesting limit exceeded");
        }
        if (seq.children.empty())
            return Node{};
        if (seq.children.size() == 1)
            return std::move(seq.children[0]);
        return seq;
    }

    bool parse_number(uint32_t& out)
    {
        const size_t start = pos_;
        uint64_t v = 0;
        while (!at_end() && peek() >= '0' && peek() <= '9')
        {
            v = v * 10 + (peek() - '0');
            if (v > 100000000)
                fail("repetition count too large");
            ++pos_;
        }
        out = static_cast<uint32_t>(v);
        return pos_ > start;
    }

    Node parse_quantifiers(Node atom, const Flags& flags)
    {
        while (true)
        {
            skip_verbose(flags);
            if (at_end())
                return atom;
            uint32_t lo = 0;
            uint32_t hi = 0;
            const char32_t c = peek();
            if (c == '*' || c == '+' || c == '?')
            {
                ++pos_;
                lo = c == '+' ? 1 : 0;
                hi = c == '?' ? 1 : Node::kUnbounded;
            }
            else if (c == '{')
            {
                ++pos_;
                skip_verbose(flags);
                if (!parse_number(lo))
                    fail("invalid repetition");
                skip_verbose(flags);
                if (peek_is(','))
                {
                    ++pos_;
                    skip_verbose(flags);
                    if (!parse_number(hi))
                        hi = Node::kUnbounded;
                }
                else
                {
                    hi = lo;
                }
                skip_verbose(flags);
                if (!peek_is('}'))
                    fail("unclosed repetition");
                ++pos_;
                if (hi != Node::kUnbounded && lo > hi)
                    fail("invalid repetition range");
            }
            else
            {
                return atom;
            }
            if (peek_is('?'))
                ++pos_; // lazy modifier: irrelevant to is_match
            Node rep;
            rep.height = atom.height + 1;
            if (rep.height > kMaxNesting)
                fail("nesting limit exceeded");
            rep.kind = Node::Kind::Repeat;
            rep.min = lo;
            rep.max = hi;
            rep.children.push_back(std::move(atom));
            atom = std::move(rep);
        }
    }

    static bool apply_flag(Flags& f, char32_t c, bool on)
    {
        switch (c)
        {
        case 'i':
            f.i = on;
            return true;
        case 'm':
            f.m = on;
            return true;
        case 's':
            f.s = on;
            return true;
        case 'x':
            f.x = on;
            return true;
        case 'U': // swap greed: no effect on is_match
        case 'u': // Unicode mode: no change in this engine
        case 'R': // CRLF mode: no change in this engine
            return true;
        default:
            return false;
        }
    }

    void skip_group_name()
    {
        size_t len = 0;
        while (!at_end() && peek() != '>')
        {
            const char32_t c = next();
            if (!(is_word(c) || c == '.' || c == '[' || c == ']'))
                fail("invalid capture group name");
            ++len;
        }
        if (at_end() || len == 0)
            fail("invalid capture group name");
        ++pos_; // '>'
    }

    // pos_ is just after '('.
    Node parse_group(Flags& flags, int depth, bool& flags_only)
    {
        Flags inner = flags;
        if (peek_is('?'))
        {
            ++pos_;
            if (peek_is(':'))
            {
                ++pos_;
            }
            else if (peek_is('P') && peek_is('<', 1))
            {
                pos_ += 2;
                skip_group_name();
            }
            else if (peek_is('<') && !peek_is('=', 1) && !peek_is('!', 1))
            {
                ++pos_;
                skip_group_name();
            }
            else if (peek_is('=') || peek_is('!') || peek_is('<'))
            {
                fail("look-around is not supported");
            }
            else
            {
                Flags updated = flags;
                bool negate = false;
                bool any = false;
                bool dangling_negation = false;
                while (true)
                {
                    const char32_t c = next();
                    if (c == ')' || c == ':')
                    {
                        if (!any || dangling_negation)
                            fail("empty flag group");
                        if (c == ')')
                        {
                            flags = updated; // rest of the enclosing group
                            flags_only = true;
                            return Node{};
                        }
                        inner = updated;
                        break;
                    }
                    if (c == '-')
                    {
                        if (negate)
                            fail("repeated flag negation");
                        negate = true;
                        dangling_negation = true;
                        continue;
                    }
                    if (!apply_flag(updated, c, !negate))
                        fail("unrecognized flag");
                    any = true;
                    dangling_negation = false;
                }
            }
        }
        Node body = parse_alt(inner, depth + 1);
        if (!peek_is(')'))
            fail("unclosed group");
        ++pos_;
        return body;
    }

    Node parse_atom(Flags& flags, int depth, bool& flags_only)
    {
        const char32_t c = next();
        switch (c)
        {
        case '(':
            return parse_group(flags, depth, flags_only);
        case '[':
        {
            CharSet s = parse_class(depth + 1, flags.i);
            return make_set(std::move(s));
        }
        case '.':
            return make_set(flags.s ? set_of({{0, kMaxCodepoint}})
                                    : set_of({{0, '\n' - 1}, {'\n' + 1, kMaxCodepoint}}));
        case '^':
            return make_assert(flags.m ? AssertKind::StartLine : AssertKind::StartText);
        case '$':
            return make_assert(flags.m ? AssertKind::EndLine : AssertKind::EndText);
        case '\\':
            return parse_escape(flags);
        case '*':
        case '+':
        case '?':
            fail("repetition operator missing expression");
        case '{':
            if (peek() >= '0' && peek() <= '9')
                fail("repetition operator missing expression");
            break;
        default:
            break;
        }
        CharSet s;
        s.add(c, c);
        if (flags.i)
            s.fold_case();
        return make_set(std::move(s));
    }

    static uint32_t hex_value(char32_t h)
    {
        if (h >= '0' && h <= '9')
            return h - '0';
        if (h >= 'a' && h <= 'f')
            return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
            return h - 'A' + 10;
        fail("invalid hexadecimal digit");
    }

    char32_t parse_hex_escape(char32_t kind)
    {
        uint32_t v = 0;
        if (peek_is('{'))
        {
            ++pos_;
            size_t n = 0;
            while (!peek_is('}'))
            {
                v = v * 16 + hex_value(next());
                if (++n > 8 || v > kMaxCodepoint)
                    fail("invalid hexadecimal escape");
            }
            ++pos_;
            if (n == 0)
                fail("empty hexadecimal escape");
        }
        else
        {
            const size_t digits = kind == 'x' ? 2 : kind == 'u' ? 4 : 8;
            for (size_t k = 0; k < digits; ++k)
                v = v * 16 + hex_value(next());
        }
        if (v > kMaxCodepoint || (v >= 0xD800 && v <= 0xDFFF))
            fail("invalid Unicode scalar value");
        return static_cast<char32_t>(v);
    }

    bool literal_escape(char32_t e, char32_t& out)
    {
        switch (e)
        {
        case 'n':
            out = '\n';
            return true;
        case 't':
            out = '\t';
            return true;
        case 'r':
            out = '\r';
            return true;
        case 'f':
            out = '\f';
            return true;
        case 'v':
            out = '\v';
            return true;
        case 'a':
            out = 0x07;
            return true;
        case 'x':
        case 'u':
        case 'U':
            out = parse_hex_escape(e);
            return true;
        default:
            break;
        }
        // Any ASCII punctuation (and space) may be escaped to stand for itself.
        if ((e >= '!' && e <= '/') || (e >= ':' && e <= '@') || (e >= '[' && e <= '`') ||
            (e >= '{' && e <= '~') || e == ' ')
        {
            out = e;
            return true;
        }
        return false;
    }

    // Escapes valid both inside and outside classes.
    bool class_escape(char32_t e, CharSet& out)
    {
        switch (e)
        {
        case 'd':
            out = digit_set();
            return true;
        case 'D':
            out = digit_set().negated();
            return true;
        case 'w':
            out = word_set();
            return true;
        case 'W':
            out = word_set().negated();
            return true;
        case 's':
            out = space_set();
            return true;
        case 'S':
            out = space_set().negated();
            return true;
        case 'p':
        case 'P':
            fail("Unicode property classes are not supported");
        case 'k':
            fail("backreferences are not supported");
        default:
            break;
        }
        if (e >= '0' && e <= '9')
            fail(e == '0' ? "octal escapes are not supported" : "backreferences are not supported");
        char32_t lit = 0;
        if (!literal_escape(e, lit))
            return false;
        out = CharSet{};
        out.add(lit, lit);
        return true;
    }

    size_t find_close_brace() const
    {
        for (size_t k = pos_ + 1; k < p_.size(); ++k)
            if (p_[k] == '}')
                return k;
        fail("unclosed brace");
    }

    Node parse_escape(const Flags& flags)
    {
        const char32_t e = next();
        switch (e)
        {
        case 'A':
            return make_assert(AssertKind::StartText);
        case 'z':
            return make_assert(AssertKind::EndText);
        case 'Z':
            fail("\\Z is not supported (use \\z)");
        case 'b':
            if (peek_is('{'))
            {
                const size_t close = find_close_brace();
                const std::u32string name(p_.begin() + static_cast<std::ptrdiff_t>(pos_ + 1),
                                          p_.begin() + static_cast<std::ptrdiff_t>(close));
                pos_ = close + 1;
                if (name == U"start")
                    return make_assert(AssertKind::WordStart);
                if (name == U"end")
                    return make_assert(AssertKind::WordEnd);
                fail("unsupported word boundary assertion");
            }
            return make_assert(AssertKind::WordBoundary);
        case 'B':
            return make_assert(AssertKind::NotWordBoundary);
        case '<':
            return make_assert(AssertKind::WordStart);
        case '>':
            return make_assert(AssertKind::WordEnd);
        default:
            break;
        }
        CharSet s;
        if (!class_escape(e, s))
            fail("unrecognized escape sequence");
        if (flags.i)
            s.fold_case();
        return make_set(std::move(s));
    }

    // ---- classes

    bool at_class_operator() const
    {
        const char32_t a = peek();
        const char32_t b = peek(1);
        return (a == '&' && b == '&') || (a == '-' && b == '-') || (a == '~' && b == '~');
    }

    // pos_ is just after '['.
    CharSet parse_class(int depth, bool fold)
    {
        if (depth > kMaxNesting)
            fail("nesting limit exceeded");
        bool negate = false;
        if (peek_is('^'))
        {
            negate = true;
            ++pos_;
        }
        CharSet result = parse_class_union(depth, /*first=*/true, fold);
        while (true)
        {
            if (at_end())
                fail("unclosed character class");
            if (peek_is(']'))
            {
                ++pos_;
                break;
            }
            const char32_t a = peek();
            pos_ += 2;
            CharSet rhs = parse_class_union(depth, /*first=*/false, fold);
            if (a == '&')
                result = result.intersect(rhs);
            else if (a == '-')
                result = result.minus(rhs);
            else
                result = result.symmetric_difference(rhs);
        }
        result.normalize();
        return negate ? result.negated() : result;
    }

    bool parse_ascii_class(CharSet& out, bool fold)
    {
        // pos_ is at ':' after '['.
        const size_t save = pos_;
        ++pos_;
        bool negate = false;
        if (peek_is('^'))
        {
            negate = true;
            ++pos_;
        }
        std::u32string name;
        while (!at_end() && peek() >= 'a' && peek() <= 'z')
            name.push_back(next());
        if (!(peek_is(':') && peek_is(']', 1)))
        {
            pos_ = save;
            return false;
        }
        pos_ += 2;
        CharSet s;
        if (name == U"alnum")
            s = set_of({{'0', '9'}, {'A', 'Z'}, {'a', 'z'}});
        else if (name == U"alpha")
            s = set_of({{'A', 'Z'}, {'a', 'z'}});
        else if (name == U"ascii")
            s = set_of({{0, 0x7F}});
        else if (name == U"blank")
            s = set_of({{'\t', '\t'}, {' ', ' '}});
        else if (name == U"cntrl")
            s = set_of({{0, 0x1F}, {0x7F, 0x7F}});
        else if (name == U"digit")
            s = digit_set();
        else if (name == U"graph")
            s = set_of({{'!', '~'}});
        else if (name == U"lower")
            s = set_of({{'a', 'z'}});
        else if (name == U"print")
            s = set_of({{' ', '~'}});
        else if (name == U"punct")
            s = set_of({{'!', '/'}, {':', '@'}, {'[', '`'}, {'{', '~'}});
        else if (name == U"space")
            s = space_set();
        else if (name == U"upper")
            s = set_of({{'A', 'Z'}});
        else if (name == U"word")
            s = word_set();
        else if (name == U"xdigit")
            s = set_of({{'0', '9'}, {'A', 'F'}, {'a', 'f'}});
        else
            fail("unknown ASCII class");
        if (fold)
            s.fold_case();
        out = negate ? s.negated() : s;
        return true;
    }

    // One class item: a single character (returns true, fills @p single) or a set.
    bool parse_class_item(int depth, CharSet& set, char32_t& single, bool fold)
    {
        const char32_t c = next();
        if (c == '[')
        {
            if (peek_is(':') && parse_ascii_class(set, fold))
                return false;
            set = parse_class(depth + 1, fold);
            return false;
        }
        if (c == '\\')
        {
            CharSet s;
            if (!class_escape(next(), s))
                fail("unrecognized escape sequence in class");
            if (s.ranges.size() == 1 && s.ranges[0].first == s.ranges[0].second)
            {
                single = s.ranges[0].first;
                return true;
            }
            set = std::move(s);
            return false;
        }
        single = c;
        return true;
    }

    CharSet parse_class_union(int depth, bool first, bool fold)
    {
        CharSet acc;
        bool leading = first;
        while (true)
        {
            if (at_end())
                fail("unclosed character class");
            if (leading && peek_is(']'))
            {
                ++pos_; // a ']' right after '[' or '[^' is a literal
                acc.add(']', ']');
                leading = false;
                continue;
            }
            leading = false;
            if (peek_is(']') || at_class_operator())
                break;
            CharSet set;
            char32_t lo = 0;
            if (!parse_class_item(depth, set, lo, fold))
            {
                acc.unite(set);
                continue;
            }
            const bool range =
                peek_is('-') && !peek_is('-', 1) && !peek_is(']', 1) && pos_ + 1 < p_.size();
            if (!range)
            {
                CharSet literal = set_of({{lo, lo}});
                if (fold)
                    literal.fold_case();
                acc.ranges.insert(acc.ranges.end(), literal.ranges.begin(), literal.ranges.end());
                continue;
            }
            ++pos_; // '-'
            CharSet set2;
            char32_t hi = 0;
            if (!parse_class_item(depth, set2, hi, fold) || hi < lo)
                fail("invalid range in character class");
            CharSet range_set = set_of({{lo, hi}});
            if (fold)
                range_set.fold_case();
            acc.ranges.insert(acc.ranges.end(), range_set.ranges.begin(), range_set.ranges.end());
        }
        acc.normalize();
        return acc;
    }

    std::vector<char32_t> p_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------- program

enum class Op : uint8_t
{
    Set,
    Split,
    Jmp,
    Assert,
    Match,
};

struct Inst
{
    Op op = Op::Match;
    uint32_t x = 0;
    uint32_t y = 0;
    AssertKind assert_kind = AssertKind::StartText;
};

} // namespace

struct LinearRegex::Program
{
    std::vector<Inst> insts;
    std::vector<CharSet> sets;
};

namespace
{

class Compiler
{
  public:
    explicit Compiler(LinearRegex::Program& prog) : prog_(prog) {}

    void compile(const Node& root)
    {
        emit(root);
        push(Inst{Op::Match});
    }

  private:
    uint32_t push(Inst inst)
    {
        if (prog_.insts.size() >= LinearRegex::kMaxProgramSize)
            fail("compiled regex exceeds size limit");
        prog_.insts.push_back(inst);
        return static_cast<uint32_t>(prog_.insts.size() - 1);
    }

    uint32_t here() const
    {
        return static_cast<uint32_t>(prog_.insts.size());
    }

    void emit(const Node& n)
    {
        // Empty repeated expressions emit no instructions, so the output-size limit alone
        // cannot bound compilation work (e.g. (?:){100000000}).
        if (++work_ > LinearRegex::kMaxProgramSize)
            fail("regex compilation exceeds work limit");
        switch (n.kind)
        {
        case Node::Kind::Empty:
            return;
        case Node::Kind::Set:
        {
            Inst inst{Op::Set};
            inst.x = static_cast<uint32_t>(prog_.sets.size());
            prog_.sets.push_back(n.set);
            push(inst);
            return;
        }
        case Node::Kind::Assert:
        {
            Inst inst{Op::Assert};
            inst.assert_kind = n.assert_kind;
            push(inst);
            return;
        }
        case Node::Kind::Concat:
            for (const auto& c : n.children)
                emit(c);
            return;
        case Node::Kind::Alt:
        {
            std::vector<uint32_t> jumps;
            for (size_t i = 0; i + 1 < n.children.size(); ++i)
            {
                const uint32_t split = push(Inst{Op::Split});
                prog_.insts[split].x = here();
                emit(n.children[i]);
                jumps.push_back(push(Inst{Op::Jmp}));
                prog_.insts[split].y = here();
            }
            emit(n.children.back());
            for (uint32_t j : jumps)
                prog_.insts[j].x = here();
            return;
        }
        case Node::Kind::Repeat:
        {
            const Node& body = n.children[0];
            for (uint32_t k = 0; k < n.min; ++k)
                emit(body);
            if (n.max == Node::kUnbounded)
            {
                const uint32_t split = push(Inst{Op::Split});
                prog_.insts[split].x = here();
                emit(body);
                Inst back{Op::Jmp};
                back.x = split;
                push(back);
                prog_.insts[split].y = here();
                return;
            }
            std::vector<uint32_t> splits;
            for (uint32_t k = n.min; k < n.max; ++k)
            {
                const uint32_t split = push(Inst{Op::Split});
                prog_.insts[split].x = here();
                splits.push_back(split);
                emit(body);
            }
            for (uint32_t s : splits)
                prog_.insts[s].y = here();
            return;
        }
        }
    }

    LinearRegex::Program& prog_;
    size_t work_ = 0;
};

// Sparse set of program counters: O(1) insert, contains and clear.
class PcSet
{
  public:
    explicit PcSet(size_t n) : sparse_(n), dense_(n) {}
    bool contains(uint32_t pc) const
    {
        const uint32_t i = sparse_[pc];
        return i < size_ && dense_[i] == pc;
    }
    void insert(uint32_t pc)
    {
        sparse_[pc] = static_cast<uint32_t>(size_);
        dense_[size_++] = pc;
    }
    void clear()
    {
        size_ = 0;
    }
    size_t size() const
    {
        return size_;
    }
    uint32_t operator[](size_t i) const
    {
        return dense_[i];
    }

  private:
    std::vector<uint32_t> sparse_;
    std::vector<uint32_t> dense_;
    size_t size_ = 0;
};

bool assertion_holds(AssertKind k, const std::vector<char32_t>& text, size_t pos)
{
    const bool prev_word = pos > 0 && is_word(text[pos - 1]);
    const bool next_word = pos < text.size() && is_word(text[pos]);
    switch (k)
    {
    case AssertKind::StartText:
        return pos == 0;
    case AssertKind::EndText:
        return pos == text.size();
    case AssertKind::StartLine:
        return pos == 0 || text[pos - 1] == '\n';
    case AssertKind::EndLine:
        return pos == text.size() || text[pos] == '\n';
    case AssertKind::WordBoundary:
        return prev_word != next_word;
    case AssertKind::NotWordBoundary:
        return prev_word == next_word;
    case AssertKind::WordStart:
        return !prev_word && next_word;
    case AssertKind::WordEnd:
        return prev_word && !next_word;
    }
    return false;
}

} // namespace

LinearRegex LinearRegex::compile(std::string_view pattern)
{
    Parser parser(decode_utf8(pattern));
    const Node root = parser.parse();
    auto prog = std::make_shared<Program>();
    Compiler(*prog).compile(root);
    return LinearRegex(std::move(prog));
}

std::size_t LinearRegex::program_size() const
{
    return program_->insts.size();
}

bool LinearRegex::search(std::string_view text) const
{
    return search(text, std::chrono::steady_clock::time_point::max()) == Result::Match;
}

LinearRegex::Result LinearRegex::search(std::string_view text_utf8,
                                        std::chrono::steady_clock::time_point deadline) const
{
    const auto& insts = program_->insts;
    const auto& sets = program_->sets;
    const std::vector<char32_t> text = decode_utf8(text_utf8);
    const size_t n = insts.size();
    PcSet current(n);
    PcSet following(n);
    PcSet visited(n); // epsilon-closure dedupe for the position being filled
    std::vector<uint32_t> stack;
    const bool timed = deadline != std::chrono::steady_clock::time_point::max();
    uint64_t work = 0;

    // Follow epsilon transitions from @p start at text position @p pos. Set instructions land
    // in @p list. Returns true when Match is reachable.
    auto add = [&](PcSet& list, uint32_t start, size_t pos) -> bool
    {
        stack.clear();
        stack.push_back(start);
        while (!stack.empty())
        {
            const uint32_t pc = stack.back();
            stack.pop_back();
            if (visited.contains(pc))
                continue;
            visited.insert(pc);
            ++work;
            const Inst& inst = insts[pc];
            switch (inst.op)
            {
            case Op::Match:
                return true;
            case Op::Jmp:
                stack.push_back(inst.x);
                break;
            case Op::Split:
                stack.push_back(inst.y);
                stack.push_back(inst.x);
                break;
            case Op::Assert:
                if (assertion_holds(inst.assert_kind, text, pos))
                    stack.push_back(pc + 1);
                break;
            case Op::Set:
                list.insert(pc);
                break;
            }
        }
        return false;
    };

    // `current` holds the threads waiting at position `pos`; `visited` is the closure dedupe
    // for that same position.
    for (size_t pos = 0;; ++pos)
    {
        if (add(current, 0, pos)) // unanchored: a new thread starts at every position
            return Result::Match;
        if (pos == text.size())
            return Result::NoMatch;
        const char32_t c = text[pos];
        visited.clear();
        for (size_t k = 0; k < current.size(); ++k)
        {
            const uint32_t pc = current[k];
            if (sets[insts[pc].x].contains(c) && add(following, pc + 1, pos + 1))
                return Result::Match;
        }
        std::swap(current, following);
        following.clear();
        if (timed && work > 65536)
        {
            work = 0;
            if (std::chrono::steady_clock::now() > deadline)
                return Result::Timeout;
        }
    }
}

} // namespace fastmcpp::util
