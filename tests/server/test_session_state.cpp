/// @file test_session_state.cpp
/// @brief Tests for session-scoped state in Context

#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/server/context.hpp"

#include <any>
#include <cassert>
#include <memory>
#include <string>

using namespace fastmcpp;
using namespace fastmcpp::server;

void test_set_and_get_session_state()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto state = std::make_shared<SessionState>();
    Context ctx(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state);

    ctx.set_session_state("counter", 42);
    auto val = ctx.get_session_state("counter");
    assert(std::any_cast<int>(val) == 42);
}

void test_shared_session_state_between_contexts()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto state = std::make_shared<SessionState>();
    Context ctx1(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state);
    Context ctx2(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state);

    ctx1.set_session_state("shared_key", std::string("hello"));
    auto val = ctx2.get_session_state("shared_key");
    assert(std::any_cast<std::string>(val) == "hello");
}

void test_independent_session_state()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto state1 = std::make_shared<SessionState>();
    auto state2 = std::make_shared<SessionState>();
    Context ctx1(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state1);
    Context ctx2(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state2);

    ctx1.set_session_state("key", 100);
    assert(!ctx2.has_session_state("key"));
}

void test_get_session_state_or_default()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto state = std::make_shared<SessionState>();
    Context ctx(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state);

    // Key doesn't exist -> returns default
    int val = ctx.get_session_state_or<int>("missing", 99);
    assert(val == 99);

    // Key exists -> returns value
    ctx.set_session_state("present", 7);
    int val2 = ctx.get_session_state_or<int>("present", 99);
    assert(val2 == 7);
}

void test_has_session_state()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    auto state = std::make_shared<SessionState>();
    Context ctx(rm, pm, std::nullopt, std::nullopt, std::nullopt, std::nullopt, state);

    assert(!ctx.has_session_state("key"));
    ctx.set_session_state("key", true);
    assert(ctx.has_session_state("key"));
}

void test_no_session_state_returns_empty()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    // Context with no session state (nullptr)
    Context ctx(rm, pm);

    // get_session_state returns empty any
    auto val = ctx.get_session_state("anything");
    assert(!val.has_value());

    // has_session_state returns false
    assert(!ctx.has_session_state("anything"));

    // get_session_state_or returns default
    int def = ctx.get_session_state_or<int>("anything", 42);
    assert(def == 42);
}

void test_set_session_state_without_ptr_throws()
{
    resources::ResourceManager rm;
    prompts::PromptManager pm;

    Context ctx(rm, pm);

    bool caught = false;
    try
    {
        ctx.set_session_state("key", 1);
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    assert(caught);
}

int main()
{
    test_set_and_get_session_state();
    test_shared_session_state_between_contexts();
    test_independent_session_state();
    test_get_session_state_or_default();
    test_has_session_state();
    test_no_session_state_returns_empty();
    test_set_session_state_without_ptr_throws();
    return 0;
}
