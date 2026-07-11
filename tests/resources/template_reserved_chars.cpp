// Reserved-character round-trip in URI-template parameter extraction.
// P4 — Python fastmcp #4368 (09490804) percent-encodes values when EXPANDING a URI
// template (building a URI from params). fastmcpp is a server: it only MATCHES incoming
// URIs and already `url_decode`s captured params (src/resources/template.cpp), and it
// never expands templates (its proxy passes URIs verbatim). So the relevant half —
// decoding percent-encoded reserved characters on the way in — is already correct.
//
// Verify-first: a `{name}` segment carrying percent-encoded reserved chars ("/" -> %2F,
// " " -> %20) must decode back to the original value.

#include "fastmcpp/resources/template.hpp"

#include <iostream>
#include <string>

using namespace fastmcpp::resources;

int main()
{
    ResourceTemplate t;
    t.uri_template = "doc://{path}/view";
    t.parse();

    // {path} matches a single segment [^/]+; the client percent-encodes reserved chars
    // (a literal "/" would otherwise be a segment boundary). Decoded value round-trips.
    auto m = t.match("doc://a%2Fb%20c/view");
    if (!m.has_value())
    {
        std::cerr << "FAIL: template did not match encoded URI" << std::endl;
        return 1;
    }
    auto it = m->find("path");
    if (it == m->end())
    {
        std::cerr << "FAIL: 'path' param not extracted" << std::endl;
        return 1;
    }
    if (it->second != "a/b c")
    {
        std::cerr << "FAIL: reserved chars not decoded, got: '" << it->second << "'" << std::endl;
        return 1;
    }

    std::cout << "PASS: URI-template reserved-char round-trip on match (P4)" << std::endl;
    return 0;
}
