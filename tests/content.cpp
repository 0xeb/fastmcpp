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
    return 0;
}
