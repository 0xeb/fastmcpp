// Example demonstrating Context.elicit() working together with the
// client-side elicitation callback (set_elicitation_callback).
//
// This is an in-process example: the server-side Context uses an
// elicitation callback that forwards the request to a fastmcpp::client::Client
// via Client::handle_notification("elicitation/request", params).
//
// The client responds using set_elicitation_callback, and the Context
// receives an ElicitationResult with the typed data.

#include "fastmcpp/client/client.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <iomanip>
#include <iostream>

using fastmcpp::Json;
using namespace fastmcpp;
using namespace fastmcpp::server;
using namespace fastmcpp::client;

int main()
{
    std::cout << "=== Context Elicitation Example (schema defaults + client callback) ===\n\n";

    // -------------------------------------------------------------------------
    // Set up a client that knows how to answer elicitation requests.
    // In a real deployment, this would be the IDE / UI process.
    // -------------------------------------------------------------------------

    Client client;

    client.set_elicitation_callback(
        [](const Json& in) -> Json
        {
            std::cout << "[client] Received elicitation request:\n";
            std::cout << "  prompt: " << in.value("prompt", std::string()) << "\n";
            if (in.contains("schema"))
                std::cout << "  schema: " << in["schema"].dump(2) << "\n";

            // In a real client, the user would fill in a form that matches the schema.
            // For this example, we just return a fixed accepted response.
            Json content = {
                {"name", "Alice"},
                {"age", 30},
                {"newsletter", true},
            };

            return Json{
                {"action", "accept"},
                {"content", content},
            };
        });

    // -------------------------------------------------------------------------
    // Server-side setup: resources/prompts managers and Context.
    // -------------------------------------------------------------------------

    resources::ResourceManager resource_mgr;
    prompts::PromptManager prompt_mgr;
    Context ctx(resource_mgr, prompt_mgr);

    // Install an elicitation callback that forwards to the client via
    // Client::handle_notification("elicitation/request", params).
    ctx.set_elicitation_callback(
        [&client](const std::string& message, const Json& schema) -> ElicitationResult
        {
            Json params = {
                {"prompt", message},
                {"schema", schema},
            };

            Json reply = client.handle_notification("elicitation/request", params);

            std::string action = reply.value("action", std::string("accept"));
            if (action == "accept")
            {
                Json content = reply.value("content", Json::object());
                return AcceptedElicitation{content};
            }
            if (action == "decline")
                return DeclinedElicitation{};
            if (action == "cancel")
                return CancelledElicitation{};

            throw Error("Unexpected elicitation action: " + action);
        });

    // -------------------------------------------------------------------------
    // Build a simple base schema with defaults and optional fields.
    // Context::elicit() will run this through get_elicitation_schema(), which:
    //   - validates it for MCP elicitation
    //   - preserves defaults
    //   - recomputes required so defaulted fields are not required.
    // -------------------------------------------------------------------------

    Json base_schema = {
        {"type", "object"},
        {"properties",
         Json{
             {"name", Json{{"type", "string"}, {"default", "Unknown"}}},
             {"age", Json{{"type", "integer"}, {"default", 25}}},
             {"newsletter", Json{{"type", "boolean"}, {"default", false}}},
         }},
    };

    std::cout << "[server] Calling Context::elicit()...\n\n";

    ElicitationResult result =
        ctx.elicit("Please confirm your profile information:", base_schema);

    if (auto* accepted = std::get_if<AcceptedElicitation>(&result))
    {
        std::cout << "[server] Elicitation accepted. Data:\n";
        std::cout << accepted->data.dump(2) << "\n\n";
    }
    else if (std::holds_alternative<DeclinedElicitation>(result))
    {
        std::cout << "[server] Elicitation was declined by the client.\n\n";
    }
    else if (std::holds_alternative<CancelledElicitation>(result))
    {
        std::cout << "[server] Elicitation was cancelled by the client.\n\n";
    }

    std::cout << "=== Example Complete ===\n";
    return 0;
}

