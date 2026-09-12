#pragma once
/// @file client/sse_stream.hpp
/// @brief Incremental Server-Sent Events block framer.
/// @details Internal to src/client. Three call sites need the same thing: turn a
///          byte stream arriving in arbitrary chunks into whole SSE events. What
///          they do with an event differs sharply -- one treats a non-JSON body
///          as an endpoint path, another wraps it as text content, a third drops
///          it -- so this deliberately stops at framing and leaves the policy to
///          the caller.

#include <cstddef>
#include <functional>
#include <string>

namespace fastmcpp::client::detail
{

/// One complete SSE event block.
struct SseEvent
{
    /// `event:` field, empty when the block carried none. Last one in the block wins.
    std::string event;

    /// `data:` fields concatenated.
    ///
    /// Folded with no separator rather than the specification's newline join,
    /// because that is what lets a JSON document split across several `data:`
    /// lines reassemble -- which servers do, and which the POST-streaming test
    /// depends on.
    std::string data;
};

/// Incremental SSE block framer.
///
/// Holds the bytes that did not yet complete an event, so a caller can hand it
/// whatever the socket produced without worrying where the chunk boundaries
/// fell -- including inside a separator or partway through a `data:` line.
class SseEventStream
{
  public:
    using Sink = std::function<void(const SseEvent&)>;

    /// Consume @p len bytes, invoking @p sink once per completed event.
    void feed(const char* data, size_t len, const Sink& sink);

    /// Emit a trailing block that never got its blank-line terminator, which is
    /// what an abruptly ended stream leaves behind. Does nothing when the buffer
    /// holds no `data:`.
    void flush(const Sink& sink);

  private:
    /// Split whatever is buffered into complete events, leaving the remainder.
    void drain(const Sink& sink);

    std::string buffer_;
};

} // namespace fastmcpp::client::detail
