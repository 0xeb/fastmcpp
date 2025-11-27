#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <cassert>

using namespace fastmcpp;

int main()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;
    pm.add("hello", prompts::Prompt{"Hello"});
    rm.register_resource(resources::Resource{Id{"file://test"}, resources::Kind::File, Json{}});

    // Context without MCP session: request metadata unavailable
    server::Context ctx_no_session(rm, pm);
    assert(!ctx_no_session.request_id().has_value());
    assert(!ctx_no_session.session_id().has_value());
    assert(!ctx_no_session.request_meta().has_value());

    // Context with MCP session metadata
    Json meta = Json{{"progressToken", "tok"}, {"client_id", "cid"}};
    server::Context ctx_with_session(rm, pm, meta, std::string{"req"}, std::string{"sess"});
    assert(ctx_with_session.request_id().value() == "req");
    assert(ctx_with_session.session_id().value() == "sess");
    assert(ctx_with_session.request_meta().has_value());
    assert(ctx_with_session.request_meta()->value("progressToken", "") == "tok");

    return 0;
}
