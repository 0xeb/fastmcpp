/// @file test_pagination.cpp
/// @brief Tests for cursor-based pagination utilities

#include "fastmcpp/util/pagination.hpp"

#include <cassert>
#include <string>
#include <vector>

using namespace fastmcpp::util::pagination;

void test_cursor_encode_decode_round_trip()
{
    auto encoded = encode_cursor(42);
    auto decoded = decode_cursor(encoded);
    assert(decoded.offset == 42);
}

void test_cursor_decode_invalid_returns_zero()
{
    // Invalid base64 / non-JSON should return offset 0
    auto decoded = decode_cursor("not_valid_base64!!!");
    assert(decoded.offset == 0);

    // Empty string
    auto decoded2 = decode_cursor("");
    assert(decoded2.offset == 0);
}

void test_paginate_sequence_basic()
{
    std::vector<int> items = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    // Page 1: first 3 items (no cursor)
    auto page1 = paginate_sequence(items, std::nullopt, 3);
    assert(page1.items.size() == 3);
    assert(page1.items[0] == 1);
    assert(page1.items[1] == 2);
    assert(page1.items[2] == 3);
    assert(page1.next_cursor.has_value());

    // Page 2: next 3 items
    auto page2 = paginate_sequence(items, page1.next_cursor, 3);
    assert(page2.items.size() == 3);
    assert(page2.items[0] == 4);
    assert(page2.items[1] == 5);
    assert(page2.items[2] == 6);
    assert(page2.next_cursor.has_value());

    // Page 3: next 3 items
    auto page3 = paginate_sequence(items, page2.next_cursor, 3);
    assert(page3.items.size() == 3);
    assert(page3.items[0] == 7);
    assert(page3.items[1] == 8);
    assert(page3.items[2] == 9);
    assert(page3.next_cursor.has_value());

    // Page 4: last item, no more pages
    auto page4 = paginate_sequence(items, page3.next_cursor, 3);
    assert(page4.items.size() == 1);
    assert(page4.items[0] == 10);
    assert(!page4.next_cursor.has_value());
}

void test_paginate_sequence_no_pagination()
{
    std::vector<int> items = {1, 2, 3};

    // page_size 0 means no pagination - return all
    auto result = paginate_sequence(items, std::nullopt, 0);
    assert(result.items.size() == 3);
    assert(!result.next_cursor.has_value());
}

void test_paginate_sequence_exact_fit()
{
    std::vector<int> items = {1, 2, 3};

    // Exactly fills one page - no next cursor
    auto result = paginate_sequence(items, std::nullopt, 3);
    assert(result.items.size() == 3);
    assert(!result.next_cursor.has_value());
}

void test_paginate_sequence_empty()
{
    std::vector<int> items;
    auto result = paginate_sequence(items, std::nullopt, 5);
    assert(result.items.empty());
    assert(!result.next_cursor.has_value());
}

void test_base64_round_trip()
{
    std::string input = "{\"offset\":99}";
    auto encoded = base64_encode(input);
    auto decoded = base64_decode(encoded);
    assert(decoded == input);
}

int main()
{
    test_cursor_encode_decode_round_trip();
    test_cursor_decode_invalid_returns_zero();
    test_paginate_sequence_basic();
    test_paginate_sequence_no_pagination();
    test_paginate_sequence_exact_fit();
    test_paginate_sequence_empty();
    test_base64_round_trip();
    return 0;
}
