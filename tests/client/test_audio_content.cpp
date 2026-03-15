#include "fastmcpp/client/types.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;

    // Parse audio content block
    Json audio_json = {{"type", "audio"}, {"data", "aGVsbG8="}, {"mimeType", "audio/wav"}};
    auto block = client::parse_content_block(audio_json);
    auto* ac = std::get_if<client::AudioContent>(&block);
    assert(ac != nullptr);
    assert(ac->data == "aGVsbG8=");
    assert(ac->mimeType == "audio/wav");

    // Parse text still works
    Json text_json = {{"type", "text"}, {"text", "hello"}};
    auto block2 = client::parse_content_block(text_json);
    assert(std::get_if<client::TextContent>(&block2) != nullptr);

    // Parse image still works
    Json img_json = {{"type", "image"}, {"data", "x"}, {"mimeType", "image/png"}};
    auto block3 = client::parse_content_block(img_json);
    assert(std::get_if<client::ImageContent>(&block3) != nullptr);

    return 0;
}
