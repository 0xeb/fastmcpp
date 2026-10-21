// Tests for fastmcpp::util::LinearRegex (Rust `regex` syntax, linear-time matching).
//
// Expected results follow the Rust regex crate, which fastmcp's RegexSearchTransform uses
// through Pydantic (fastmcp_slim/fastmcp/server/transforms/search/regex.py).

#include "fastmcpp/util/linear_regex.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using fastmcpp::util::LinearRegex;

namespace
{

bool compiles(const std::string& pattern)
{
    try
    {
        (void)LinearRegex::compile(pattern);
        return true;
    }
    catch (const LinearRegex::Error&)
    {
        return false;
    }
}

bool matches(const std::string& pattern, const std::string& text)
{
    return LinearRegex::compile(pattern).search(text);
}

struct Case
{
    const char* pattern;
    const char* text;
    bool expected;
};

void check(const std::vector<Case>& cases)
{
    for (const auto& c : cases)
    {
        const bool got = matches(c.pattern, c.text);
        if (got != c.expected)
        {
            std::cerr << "\n  pattern '" << c.pattern << "' on '" << c.text << "': got " << got
                      << ", expected " << c.expected << "\n";
            assert(false);
        }
    }
}

void test_literals_and_operators()
{
    std::cout << "test_literals_and_operators..." << std::flush;
    check({
        {"abc", "xxabcxx", true},
        {"abc", "ab", false},
        {"", "", true},
        {"", "anything", true},
        {"a|b", "b", true},
        {"a|", "zzz", true}, // empty alternative matches
        {"colou?r", "color", true},
        {"colou?r", "colour", true},
        {"ab*c", "ac", true},
        {"ab+c", "ac", false},
        {"ab+c", "abbbc", true},
        {"a{3}", "aa", false},
        {"a{3}", "aaa", true},
        {"^a{2,}$", "aaaa", true},
        {"^a{2,3}$", "aaaa", false},
        {"^a{0}b$", "b", true},
        {"(ab)+", "xababx", true},
        {"(?:ab)+c", "ababc", true},
        {"a.c", "abc", true},
        {"a.c", "a\nc", false},
        {"(?s)a.c", "a\nc", true},
        {"^(a|b)+$", "abba", true}, // repeated alternation is accepted, as in Rust
        {"(?P<word>email)", "send email", true},
        {"(?<word>email)", "send email", true},
        {"a.*?b", "axxb", true},
    });
    std::cout << " OK\n";
}

void test_anchors_and_boundaries()
{
    std::cout << "test_anchors_and_boundaries..." << std::flush;
    check({
        {"^send", "send_email", true},
        {"^email", "send_email", false},
        {"email$", "send_email", true},
        {"email$", "send_email\n", false}, // Rust `$` is end of text, not before a final \n
        {"(?m)^b$", "a\nb\nc", true},
        {"^b$", "a\nb\nc", false},
        {"\\Asend", "send", true},
        {"email\\z", "email", true},
        {"\\bmail", "send mail", true},
        {"\\bmail", "email", false},
        {"\\Bmail", "email", true},
        {"\\<send\\>", "a send b", true},
        {"\\<end", "send", false},
        {"\\b{start}send\\b{end}", "x send y", true},
    });
    std::cout << " OK\n";
}

void test_classes()
{
    std::cout << "test_classes..." << std::flush;
    check({
        {"^[a-c]+$", "abcab", true},
        {"^[a-c]+$", "abcd", false},
        {"[^a-z]", "abc", false},
        {"[^a-z]", "ab1", true},
        {"[]_]email", "_email", true},
        {"[]_]email", "]email", true},
        {"[a-]", "-", true},
        {"\\d+", "id 42", true},
        {"^\\D+$", "abc", true},
        {"^\\w+$", "snake_case1", true},
        {"\\s", "nospace", false},
        {"\\S", "   x ", true},
        {"[\\d_]", "_", true},
        {"[[:alpha:]]+[[:digit:]]", "abc9", true},
        {"[[:^digit:]]", "123", false},
        {"[a-z&&[^aeiou]]", "aei", false}, // intersection
        {"[a-z&&[^aeiou]]", "aeb", true},
        {"[a-z--[b-y]]", "m", false}, // difference
        {"[a-z--[b-y]]", "z", true},
        {"[a-c~~b-d]", "b", false}, // symmetric difference
        {"[a-c~~b-d]", "d", true},
        {"[[a-c][x-z]]", "y", true}, // nested class
        {"\\x41", "A", true},
        {"\\x{1F600}", "\xF0\x9F\x98\x80", true},
        {"\\u00e9", "caf\xC3\xA9", true},
        {"^.$", "\xC3\xA9", true}, // `.` is one code point, not one byte
        {"\\.", "a.b", true},
        {"\\-\\[\\]", "-[]", true},
    });
    std::cout << " OK\n";
}

void test_flags()
{
    std::cout << "test_flags..." << std::flush;
    check({
        {"(?i)EMAIL", "send_email", true},
        {"EMAIL", "send_email", false},
        {"(?i)[A-C]", "b", true},
        {"(?i)[^a]", "A", false},
        {"(?i)[^a]", "a", false},
        {"(?i)[^a]", "b", true},
        {"(?i)[a-z&&[^aeiou]]", "A", false},
        {"(?i)[a-z--[B-Y]]", "m", false},
        {"(?i)[[:^lower:]]", "A", false},
        {"(?i)\\x41", "a", true},
        {"(?i)(?-i:SEND)", "send", false},
        {"(?i)(?-i:send)", "send", true},
        {"(?i:A)b", "aB", false},
        {"a(?i)b|c", "C", true}, // flags apply to the rest of the group, other branches too
        {"(?x) s e n d  # comment", "send", true},
        {"(?x)s\\ e", "s e", true},
        {"(?U)a+", "aaa", true},
        {"(?is)A.B", "a\nb", true},
    });
    std::cout << " OK\n";
}

void test_rejections_match_rust()
{
    std::cout << "test_rejections_match_rust..." << std::flush;
    for (const char* bad : {"send(?=_email)",
                            "send(?!x)",
                            "(?<=send_)email",
                            "(?<!x)y",
                            "(email).*\\1",
                            "\\k<name>",
                            ".*\\Z",
                            "(?a)email",
                            "(?)x",
                            "(?-)x",
                            "[invalid",
                            "(unclosed",
                            "unopened)",
                            "*a",
                            "a{3,2}",
                            "a{",
                            "\\0",
                            "\\q",
                            "\\pL",
                            "\\p{Greek}",
                            "[z-a]",
                            "\\x{110000}",
                            "(?P<>x)"})
    {
        if (compiles(bad))
        {
            std::cerr << "\n  expected a compile error for '" << bad << "'\n";
            assert(false);
        }
    }
    // Size limit, as Rust's compiled-size limit.
    assert(!compiles("(?:a{1000}){1000}"));
    assert(compiles("(?:[^z]?){9000}zzz"));
    // The AST depth includes repeated quantifiers, even without nested parentheses.
    assert(!compiles("a" + std::string(1000, '*')));
    // Empty bodies do not grow the program; a separate work budget must stop expansion.
    const auto started = std::chrono::steady_clock::now();
    assert(!compiles("(?:){100000000}"));
    assert(!compiles("(?:a{0}){100000000}"));
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    std::cout << " OK\n";
}

void test_linear_time()
{
    std::cout << "test_linear_time..." << std::flush;
    using clock = std::chrono::steady_clock;
    // Exponential for a backtracking engine; linear here.
    const std::string text = std::string(10000, 'a') + "!";
    const auto started = clock::now();
    assert(!matches("(a|aa)+$", text));
    assert(!matches("(a+)+$", text));
    assert(!matches("^(a|a)*b", text));
    assert(matches("(a|aa)+!$", text));
    const auto elapsed = clock::now() - started;
    std::cout << " (" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " ms)";
    assert(elapsed < std::chrono::seconds(5));
    std::cout << " OK\n";
}

void test_deadline()
{
    std::cout << "test_deadline..." << std::flush;
    using clock = std::chrono::steady_clock;
    // Linear but slow: a large counted repetition over a long text.
    const auto re = LinearRegex::compile("(?:[^z]?){9000}zzz");
    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "lorem ";
    const auto started = clock::now();
    const auto result = re.search(text, started + std::chrono::milliseconds(50));
    const auto elapsed = clock::now() - started;
    assert(result == LinearRegex::Result::Timeout);
    assert(elapsed < std::chrono::seconds(2));
    // An already expired deadline still lets a quick match through or times out; never hangs.
    const auto quick = LinearRegex::compile("lorem").search(text, clock::now());
    assert(quick != LinearRegex::Result::NoMatch);
    std::cout << " OK\n";
}

} // namespace

int main()
{
    test_literals_and_operators();
    test_anchors_and_boundaries();
    test_classes();
    test_flags();
    test_rejections_match_rust();
    test_linear_time();
    test_deadline();
    std::cout << "All LinearRegex tests passed\n";
    return 0;
}
