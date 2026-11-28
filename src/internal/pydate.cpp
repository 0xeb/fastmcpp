#include "pydate.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <vector>

namespace fastmcpp::internal::pydate
{

namespace
{

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' ||
           (c >= 0x1C && c <= 0x1F);
}

std::string strip(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b]))
        ++b;
    while (e > b && is_space(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split_ws(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (is_space(c))
        {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

std::vector<std::string> split_on(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        const auto pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos)
            return out;
        start = pos + 1;
    }
}

long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

} // namespace

std::optional<long long> py_int(const std::string& raw)
{
    const std::string text = strip(raw);
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        negative = text[i++] == '-';
    if (i >= text.size())
        return std::nullopt;
    long long value = 0;
    bool prev_digit = false;
    for (; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '_')
        {
            if (!prev_digit || i + 1 >= text.size())
                return std::nullopt;
            prev_digit = false;
            continue;
        }
        if (c < '0' || c > '9')
            return std::nullopt;
        if (value > (std::numeric_limits<long long>::max() - (c - '0')) / 10)
            return std::nullopt;
        value = value * 10 + (c - '0');
        prev_digit = true;
    }
    return negative ? -value : value;
}

std::optional<double> parsedate_timestamp(const std::string& text)
{
    static const std::vector<std::string> months = {
        "jan", "feb",  "mar",  "apr",    "may",       "jun",      "jul",      "aug",
        "sep", "oct",  "nov",  "dec",    "january",   "february", "march",    "april",
        "may", "june", "july", "august", "september", "october",  "november", "december"};
    static const std::vector<std::string> days = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    static const std::map<std::string, int> zones = {
        {"UT", 0},     {"UTC", 0},    {"GMT", 0},    {"Z", 0},      {"AST", -400},
        {"ADT", -300}, {"EST", -500}, {"EDT", -400}, {"CST", -600}, {"CDT", -500},
        {"MST", -700}, {"MDT", -600}, {"PST", -800}, {"PDT", -700}};

    std::vector<std::string> data = split_ws(text);
    if (data.empty())
        return std::nullopt;
    if (data[0].back() == ',' || std::find(days.begin(), days.end(), lower(data[0])) != days.end())
        data.erase(data.begin());
    else if (const auto i = data[0].rfind(','); i != std::string::npos)
        data[0] = data[0].substr(i + 1);
    if (data.size() == 3)
    {
        auto stuff = split_on(data[0], '-');
        if (stuff.size() == 3)
        {
            stuff.insert(stuff.end(), data.begin() + 1, data.end());
            data = std::move(stuff);
        }
    }
    if (data.size() == 4)
    {
        const std::string s = data[3];
        auto i = s.find('+');
        if (i == std::string::npos)
            i = s.find('-');
        if (i != std::string::npos && i > 0)
        {
            data[3] = s.substr(0, i);
            data.push_back(s.substr(i));
        }
        else
            data.push_back("");
    }
    if (data.size() < 5)
        return std::nullopt;
    std::string dd = data[0], mm = data[1], yy = data[2], tm = data[3], tz = data[4];
    if (dd.empty() || mm.empty() || yy.empty())
        return std::nullopt;
    mm = lower(mm);
    auto month_it = std::find(months.begin(), months.end(), mm);
    if (month_it == months.end())
    {
        std::swap(dd, mm);
        mm = lower(mm);
        month_it = std::find(months.begin(), months.end(), mm);
        if (month_it == months.end())
            return std::nullopt;
    }
    int month = static_cast<int>(month_it - months.begin()) + 1;
    if (month > 12)
        month -= 12;
    if (dd.back() == ',')
        dd.pop_back();
    if (const auto i = yy.find(':'); i != std::string::npos && i > 0)
        std::swap(yy, tm);
    if (yy.empty())
        return std::nullopt;
    if (yy.back() == ',')
    {
        yy.pop_back();
        if (yy.empty())
            return std::nullopt;
    }
    if (!(yy[0] >= '0' && yy[0] <= '9'))
        std::swap(yy, tz);
    if (tm.empty())
        return std::nullopt;
    if (tm.back() == ',')
        tm.pop_back();
    std::vector<std::string> t = split_on(tm, ':');
    if (t.size() == 1 && tm.find('.') != std::string::npos)
        t = split_on(tm, '.');
    std::string thh, tmm, tss = "0";
    if (t.size() == 2)
    {
        thh = t[0];
        tmm = t[1];
    }
    else if (t.size() == 3)
    {
        thh = t[0];
        tmm = t[1];
        tss = t[2];
    }
    else
        return std::nullopt;
    const auto year = py_int(yy), day = py_int(dd), hour = py_int(thh), minute = py_int(tmm),
               second = py_int(tss);
    if (!year || !day || !hour || !minute || !second)
        return std::nullopt;
    long long y = *year;
    if (y < 100)
        y += y > 68 ? 1900 : 2000;

    long long offset = 0;
    const std::string zone = upper(tz);
    if (const auto z = zones.find(zone); z != zones.end())
        offset = z->second;
    else if (const auto v = py_int(zone))
        offset = *v;
    if (offset != 0)
    {
        const long long sign = offset < 0 ? -1 : 1;
        const long long a = offset < 0 ? -offset : offset;
        if (a > 100000000)
            return std::nullopt;
        offset = sign * ((a / 100) * 3600 + (a % 100) * 60);
    }
    if (offset <= -86400 || offset >= 86400)
        return std::nullopt;

    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1 || y > 9999)
        return std::nullopt;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int dim = mdays[month - 1] + (month == 2 && leap ? 1 : 0);
    if (*day < 1 || *day > dim || *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59 ||
        *second < 0 || *second > 59)
        return std::nullopt;
    const long long secs =
        days_from_civil(y, static_cast<unsigned>(month), static_cast<unsigned>(*day)) * 86400 +
        *hour * 3600 + *minute * 60 + *second - offset;
    return static_cast<double>(secs);
}

} // namespace fastmcpp::internal::pydate
