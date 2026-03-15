#include "fastmcpp/content.hpp"

#include <cassert>

int main()
{
    using namespace fastmcpp;
    TextContent t{"text", "Hello"};
    Json jt = t;
    assert(jt.at("type") == "text");
    assert(jt.at("text") == "Hello");

    ImageContent img;
    img.data = "BASE64DATA";
    img.mimeType = "image/png";
    Json ji = img;
    assert(ji.at("type") == "image");
    assert(ji.at("data") == "BASE64DATA");
    assert(ji.at("mimeType") == "image/png");

    AudioContent audio;
    audio.data = "AQID";
    audio.mimeType = "audio/wav";
    Json ja = audio;
    assert(ja.at("type") == "audio");
    assert(ja.at("data") == "AQID");
    assert(ja.at("mimeType") == "audio/wav");
    return 0;
}
