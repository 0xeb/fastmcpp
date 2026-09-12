// SseEventStream: the incremental SSE block framer shared by the streaming
// client paths.
//
// The behaviours asserted here are the ones the call sites actually depend on,
// several of which are not what a naive reading of the SSE spec would produce --
// notably that multi-line `data:` folds with no separator, because that is what
// lets a JSON document split across lines reassemble.

#include "../../src/client/sse_stream.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using fastmcpp::client::detail::SseEvent;
using fastmcpp::client::detail::SseEventStream;

namespace
{

/// Collects events, and records how many feed() calls produced them.
struct Collector
{
    std::vector<SseEvent> events;

    SseEventStream::Sink sink()
    {
        return [this](const SseEvent& e) { events.push_back(e); };
    }
};

void feed(SseEventStream& s, Collector& c, const std::string& bytes)
{
    s.feed(bytes.data(), bytes.size(), c.sink());
}

void test_basic_framing()
{
    std::cout << "Test 1: whole events are framed and fields parsed...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "event: message\ndata: {\"a\":1}\n\n");

    assert(c.events.size() == 1);
    assert(c.events[0].event == "message");
    assert(c.events[0].data == "{\"a\":1}");

    std::cout << "  [PASS] event: and data: extracted\n";
}

void test_split_across_chunks()
{
    std::cout << "Test 2: an event split across arbitrary chunks reassembles...\n";

    SseEventStream stream;
    Collector c;

    // Split inside the data line, and again inside the "\n\n" separator.
    feed(stream, c, "data: {\"n\":");
    assert(c.events.empty() && "no event until the block terminates");
    feed(stream, c, "1}\n");
    assert(c.events.empty());
    feed(stream, c, "\n");

    assert(c.events.size() == 1);
    assert(c.events[0].data == "{\"n\":1}");

    std::cout << "  [PASS] Residual bytes carried across chunk boundaries\n";
}

void test_multiline_data_folds_without_separator()
{
    std::cout << "Test 3: multi-line data folds with no separator...\n";

    SseEventStream stream;
    Collector c;
    // This is the shape a server produces when it splits a JSON document; a
    // newline joiner here would make it unparseable.
    feed(stream, c, "data: {\"n\":\ndata: 2}\n\n");

    assert(c.events.size() == 1);
    assert(c.events[0].data == "{\"n\":2}");

    std::cout << "  [PASS] data: lines concatenated, not newline-joined\n";
}

void test_crlf_framing()
{
    std::cout << "Test 4: CRLF-framed streams parse...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "event: endpoint\r\ndata: /messages?session_id=abc\r\n\r\n");

    assert(c.events.size() == 1);
    assert(c.events[0].event == "endpoint");
    assert(c.events[0].data == "/messages?session_id=abc" && "trailing CR must be stripped");

    std::cout << "  [PASS] \\r\\n\\r\\n separator and trailing CR handled\n";
}

void test_multiple_events_in_one_chunk()
{
    std::cout << "Test 5: several events in a single chunk...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "data: one\n\ndata: two\n\ndata: three\n\n");

    assert(c.events.size() == 3);
    assert(c.events[0].data == "one");
    assert(c.events[1].data == "two");
    assert(c.events[2].data == "three");

    std::cout << "  [PASS] All three delivered in order\n";
}

void test_empty_blocks_are_skipped()
{
    std::cout << "Test 6: blocks with no data are not delivered...\n";

    SseEventStream stream;
    Collector c;
    // A comment-only block and a bare keep-alive newline pair.
    feed(stream, c, ": keep-alive\n\n\n\ndata: real\n\n");

    assert(c.events.size() == 1);
    assert(c.events[0].data == "real");

    std::cout << "  [PASS] Only the block carrying data was emitted\n";
}

void test_flush_emits_unterminated_tail()
{
    std::cout << "Test 7: flush() delivers a block with no trailing blank line...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "data: first\n\ndata: truncated");

    assert(c.events.size() == 1 && "the unterminated block must not be delivered by feed()");

    stream.flush(c.sink());
    assert(c.events.size() == 2);
    assert(c.events[1].data == "truncated" && "flush must still strip the data: prefix");

    // A second flush has nothing left to give.
    stream.flush(c.sink());
    assert(c.events.size() == 2);

    std::cout << "  [PASS] Tail parsed rather than handed over raw\n";
}

void test_one_space_stripped_not_trimmed()
{
    std::cout << "Test 8: exactly one leading space is stripped...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "data:  two-spaces\n\n");

    assert(c.events.size() == 1);
    assert(c.events[0].data == " two-spaces" && "only the first space belongs to the framing");

    std::cout << "  [PASS] Second space preserved as content\n";
}

void test_last_event_field_wins()
{
    std::cout << "Test 9: the last event: in a block wins...\n";

    SseEventStream stream;
    Collector c;
    feed(stream, c, "event: first\nevent: second\ndata: x\n\n");

    assert(c.events.size() == 1);
    assert(c.events[0].event == "second");

    std::cout << "  [PASS] Later event: replaced the earlier one\n";
}

} // namespace

int main()
{
    std::cout << "=== SSE event stream tests ===\n\n";

    test_basic_framing();
    test_split_across_chunks();
    test_multiline_data_folds_without_separator();
    test_crlf_framing();
    test_multiple_events_in_one_chunk();
    test_empty_blocks_are_skipped();
    test_flush_emits_unterminated_tail();
    test_one_space_stripped_not_trimmed();
    test_last_event_field_wins();

    std::cout << "\n=== All SSE event stream tests passed ===\n";
    return 0;
}
