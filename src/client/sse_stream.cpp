#include "sse_stream.hpp"

namespace fastmcpp::client::detail
{
namespace
{

/// Parse one already-delimited block into its `event:` and folded `data:`.
SseEvent parse_block(const std::string& chunk)
{
    SseEvent event;

    size_t line_start = 0;
    while (line_start < chunk.size())
    {
        const size_t line_end = chunk.find('\n', line_start);
        std::string line = chunk.substr(
            line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);

        // CRLF streams leave the carriage return on every line.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.rfind("event:", 0) == 0)
        {
            event.event = line.substr(6);
            // Exactly one optional space after the colon, per the SSE grammar --
            // not a general trim, which would eat meaningful leading whitespace.
            if (!event.event.empty() && event.event[0] == ' ')
                event.event.erase(0, 1);
        }
        else if (line.rfind("data:", 0) == 0)
        {
            std::string data_part = line.substr(5);
            if (!data_part.empty() && data_part[0] == ' ')
                data_part.erase(0, 1);
            event.data += data_part;
        }
        // `id:`, `retry:` and comment lines are not used by any caller here.

        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
    }

    return event;
}

} // namespace

void SseEventStream::drain(const Sink& sink)
{
    size_t pos = 0;
    while (true)
    {
        // Both separators occur in practice: "\n\n" from most servers, "\r\n\r\n"
        // from those framing strictly as HTTP. "\n\n" is not a substring of
        // "\r\n\r\n", so trying it first is safe.
        size_t sep = buffer_.find("\n\n", pos);
        size_t sep_len = 2;
        if (sep == std::string::npos)
        {
            sep = buffer_.find("\r\n\r\n", pos);
            sep_len = 4;
        }
        if (sep == std::string::npos)
            break;

        const SseEvent event = parse_block(buffer_.substr(pos, sep - pos));
        pos = sep + sep_len;

        if (!event.data.empty())
            sink(event);
    }

    if (pos > 0)
        buffer_.erase(0, pos);
}

void SseEventStream::feed(const char* data, size_t len, const Sink& sink)
{
    buffer_.append(data, len);
    drain(sink);
}

void SseEventStream::flush(const Sink& sink)
{
    drain(sink);

    if (buffer_.empty())
        return;

    // Whatever is left never got its blank line. Parse it the same way rather
    // than handing the caller raw bytes: a stream that ends without a trailing
    // terminator still carried a real event.
    const SseEvent event = parse_block(buffer_);
    buffer_.clear();

    if (!event.data.empty())
        sink(event);
}

} // namespace fastmcpp::client::detail
