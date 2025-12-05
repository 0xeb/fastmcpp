/// @file test_context_sampling.cpp
/// @brief Tests for Context sampling functionality

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_sampling_types_defaults()
{
    std::cout << "  test_sampling_types_defaults... " << std::flush;

    // SamplingMessage defaults
    SamplingMessage msg;
    assert(msg.role.empty());
    assert(msg.content.empty());

    // SamplingMessage with values
    SamplingMessage msg2{"user", "Hello"};
    assert(msg2.role == "user");
    assert(msg2.content == "Hello");

    // SamplingParams defaults (all optional)
    SamplingParams params;
    assert(!params.system_prompt.has_value());
    assert(!params.temperature.has_value());
    assert(!params.max_tokens.has_value());
    assert(!params.model_preferences.has_value());

    // SamplingResult defaults
    SamplingResult result;
    assert(result.type.empty());
    assert(result.content.empty());
    assert(!result.mime_type.has_value());

    // SamplingResult with values
    SamplingResult result2{"text", "Response", std::nullopt};
    assert(result2.type == "text");
    assert(result2.content == "Response");

    std::cout << "PASSED\n";
}

void test_has_sampling()
{
    std::cout << "  test_has_sampling... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    // No callback set initially
    assert(!ctx.has_sampling());

    // Set callback
    ctx.set_sampling_callback(
        [](const std::vector<SamplingMessage>&, const SamplingParams&) -> SamplingResult
        { return {"text", "response", std::nullopt}; });

    assert(ctx.has_sampling());

    std::cout << "PASSED\n";
}

void test_sample_without_callback_throws()
{
    std::cout << "  test_sample_without_callback_throws... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    bool threw = false;
    try
    {
        ctx.sample("Hello");
    }
    catch (const std::runtime_error& e)
    {
        threw = true;
        std::string msg = e.what();
        assert(msg.find("Sampling not available") != std::string::npos);
    }
    assert(threw);

    std::cout << "PASSED\n";
}

void test_sample_string_input()
{
    std::cout << "  test_sample_string_input... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    std::vector<SamplingMessage> captured_messages;
    SamplingParams captured_params;

    ctx.set_sampling_callback(
        [&](const std::vector<SamplingMessage>& msgs,
            const SamplingParams& params) -> SamplingResult
        {
            captured_messages = msgs;
            captured_params = params;
            return {"text", "Hello back!", std::nullopt};
        });

    auto result = ctx.sample("Hello");

    // Verify message was converted to vector
    assert(captured_messages.size() == 1);
    assert(captured_messages[0].role == "user");
    assert(captured_messages[0].content == "Hello");

    // Verify result
    assert(result.type == "text");
    assert(result.content == "Hello back!");

    std::cout << "PASSED\n";
}

void test_sample_message_vector()
{
    std::cout << "  test_sample_message_vector... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    std::vector<SamplingMessage> captured_messages;

    ctx.set_sampling_callback(
        [&](const std::vector<SamplingMessage>& msgs, const SamplingParams&) -> SamplingResult
        {
            captured_messages = msgs;
            return {"text", "Got it", std::nullopt};
        });

    std::vector<SamplingMessage> messages = {
        {"user", "First message"}, {"assistant", "First response"}, {"user", "Follow up"}};

    auto result = ctx.sample(messages);

    // Verify all messages passed through
    assert(captured_messages.size() == 3);
    assert(captured_messages[0].role == "user");
    assert(captured_messages[0].content == "First message");
    assert(captured_messages[1].role == "assistant");
    assert(captured_messages[1].content == "First response");
    assert(captured_messages[2].role == "user");
    assert(captured_messages[2].content == "Follow up");

    std::cout << "PASSED\n";
}

void test_sample_with_params()
{
    std::cout << "  test_sample_with_params... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    SamplingParams captured_params;

    ctx.set_sampling_callback(
        [&](const std::vector<SamplingMessage>&, const SamplingParams& params) -> SamplingResult
        {
            captured_params = params;
            return {"text", "Response", std::nullopt};
        });

    SamplingParams params;
    params.system_prompt = "You are helpful";
    params.temperature = 0.7f;
    params.max_tokens = 100;
    params.model_preferences = std::vector<std::string>{"claude-3", "gpt-4"};

    ctx.sample("Hello", params);

    assert(captured_params.system_prompt.has_value());
    assert(captured_params.system_prompt.value() == "You are helpful");
    assert(captured_params.temperature.has_value());
    assert(captured_params.temperature.value() == 0.7f);
    assert(captured_params.max_tokens.has_value());
    assert(captured_params.max_tokens.value() == 100);
    assert(captured_params.model_preferences.has_value());
    assert(captured_params.model_preferences.value().size() == 2);
    assert(captured_params.model_preferences.value()[0] == "claude-3");

    std::cout << "PASSED\n";
}

void test_sample_text_convenience()
{
    std::cout << "  test_sample_text_convenience... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    ctx.set_sampling_callback(
        [](const std::vector<SamplingMessage>&, const SamplingParams&) -> SamplingResult
        { return {"text", "Just the text", std::nullopt}; });

    // sample_text returns just the content string
    std::string result = ctx.sample_text("What is 2+2?");
    assert(result == "Just the text");

    std::cout << "PASSED\n";
}

void test_sample_image_result()
{
    std::cout << "  test_sample_image_result... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    ctx.set_sampling_callback(
        [](const std::vector<SamplingMessage>&, const SamplingParams&) -> SamplingResult
        { return {"image", "base64encodeddata", std::string{"image/png"}}; });

    auto result = ctx.sample("Generate an image");
    assert(result.type == "image");
    assert(result.content == "base64encodeddata");
    assert(result.mime_type.has_value());
    assert(result.mime_type.value() == "image/png");

    std::cout << "PASSED\n";
}

void test_sample_audio_result()
{
    std::cout << "  test_sample_audio_result... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    ctx.set_sampling_callback(
        [](const std::vector<SamplingMessage>&, const SamplingParams&) -> SamplingResult
        { return {"audio", "audiodata", std::string{"audio/mp3"}}; });

    auto result = ctx.sample("Read this aloud");
    assert(result.type == "audio");
    assert(result.content == "audiodata");
    assert(result.mime_type.value() == "audio/mp3");

    std::cout << "PASSED\n";
}

void test_e2e_tool_uses_sampling()
{
    std::cout << "  test_e2e_tool_uses_sampling... " << std::flush;

    resources::ResourceManager rm;
    prompts::PromptManager pm;
    Context ctx(rm, pm);

    // Simulate LLM responses
    int call_count = 0;
    ctx.set_sampling_callback(
        [&](const std::vector<SamplingMessage>& msgs, const SamplingParams&) -> SamplingResult
        {
            call_count++;
            // Return different responses based on input
            if (msgs.back().content.find("summarize") != std::string::npos)
                return {"text", "Summary: The document discusses testing.", std::nullopt};
            return {"text", "Default response", std::nullopt};
        });

    // Simulate tool that uses sampling
    auto analyze_document = [&ctx](const std::string& doc) -> std::string
    {
        if (!ctx.has_sampling())
            return "Error: Sampling not available";

        // First ask LLM to summarize
        auto summary = ctx.sample_text("Please summarize: " + doc);

        return "Analysis complete. " + summary;
    };

    std::string result = analyze_document("Test document content");
    assert(result.find("Summary:") != std::string::npos);
    assert(call_count == 1);

    std::cout << "PASSED\n";
}

int main()
{
    std::cout << "Context Sampling Tests\n";
    std::cout << "======================\n";

    try
    {
        test_sampling_types_defaults();
        test_has_sampling();
        test_sample_without_callback_throws();
        test_sample_string_input();
        test_sample_message_vector();
        test_sample_with_params();
        test_sample_text_convenience();
        test_sample_image_result();
        test_sample_audio_result();
        test_e2e_tool_uses_sampling();

        std::cout << "\nAll tests passed!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}
