#include "fastmcpp/client/types.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/prompts/manager.hpp"
#include "fastmcpp/prompts/prompt.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace fastmcpp::prompts;
using namespace fastmcpp;

// ============================================================================
// TestPromptRender - Basic prompt rendering tests
// ============================================================================

void test_basic_template()
{
    std::cout << "test_basic_template...\n";
    Prompt p{"Hello {name}!"};
    auto out = p.render({{"name", "World"}});
    assert(out == "Hello World!");
    std::cout << "  [PASS]\n";
}

void test_template_string()
{
    std::cout << "test_template_string...\n";
    Prompt p{"Hello {name}!"};
    assert(p.template_string() == "Hello {name}!");
    std::cout << "  [PASS]\n";
}

void test_multiple_variables()
{
    std::cout << "test_multiple_variables...\n";
    Prompt p{"{greeting} {name}, you are {age} years old."};
    auto out = p.render({{"greeting", "Hello"}, {"name", "Alice"}, {"age", "30"}});
    assert(out == "Hello Alice, you are 30 years old.");
    std::cout << "  [PASS]\n";
}

void test_repeated_variable()
{
    std::cout << "test_repeated_variable...\n";
    Prompt p{"{name} loves {name}'s job."};
    auto out = p.render({{"name", "Bob"}});
    assert(out == "Bob loves Bob's job.");
    std::cout << "  [PASS]\n";
}

void test_no_variables()
{
    std::cout << "test_no_variables...\n";
    Prompt p{"Hello World!"};
    auto out = p.render({});
    assert(out == "Hello World!");
    std::cout << "  [PASS]\n";
}

void test_empty_template()
{
    std::cout << "test_empty_template...\n";
    Prompt p{""};
    auto out = p.render({});
    assert(out == "");
    std::cout << "  [PASS]\n";
}

void test_only_variable()
{
    std::cout << "test_only_variable...\n";
    Prompt p{"{message}"};
    auto out = p.render({{"message", "Hello World"}});
    assert(out == "Hello World");
    std::cout << "  [PASS]\n";
}

void test_empty_variable_value()
{
    std::cout << "test_empty_variable_value...\n";
    Prompt p{"Hello {name}!"};
    auto out = p.render({{"name", ""}});
    assert(out == "Hello !");
    std::cout << "  [PASS]\n";
}

void test_numeric_values()
{
    std::cout << "test_numeric_values...\n";
    Prompt p{"The answer is {value}."};
    auto out = p.render({{"value", "42"}});
    assert(out == "The answer is 42.");
    std::cout << "  [PASS]\n";
}

void test_special_characters_in_value()
{
    std::cout << "test_special_characters_in_value...\n";
    Prompt p{"Email: {email}"};
    auto out = p.render({{"email", "user@example.com"}});
    assert(out == "Email: user@example.com");
    std::cout << "  [PASS]\n";
}

void test_json_in_value()
{
    std::cout << "test_json_in_value...\n";
    Prompt p{"Data: {data}"};
    auto out = p.render({{"data", R"({"key": "value"})"}});
    assert(out == R"(Data: {"key": "value"})");
    std::cout << "  [PASS]\n";
}

void test_multiline_template()
{
    std::cout << "test_multiline_template...\n";
    Prompt p{"Line 1: {a}\nLine 2: {b}"};
    auto out = p.render({{"a", "first"}, {"b", "second"}});
    assert(out == "Line 1: first\nLine 2: second");
    std::cout << "  [PASS]\n";
}

void test_adjacent_variables()
{
    std::cout << "test_adjacent_variables...\n";
    Prompt p{"{first}{second}{third}"};
    auto out = p.render({{"first", "A"}, {"second", "B"}, {"third", "C"}});
    assert(out == "ABC");
    std::cout << "  [PASS]\n";
}

// ============================================================================
// TestPromptManager - Prompt manager tests
// ============================================================================

void test_manager_add_and_get()
{
    std::cout << "test_manager_add_and_get...\n";
    PromptManager pm;
    pm.add("greet", Prompt{"Hello {name}!"});
    assert(pm.has("greet"));
    auto out = pm.get("greet").render({{"name", "Ada"}});
    assert(out == "Hello Ada!");
    std::cout << "  [PASS]\n";
}

void test_manager_has()
{
    std::cout << "test_manager_has...\n";
    PromptManager pm;
    assert(!pm.has("nonexistent"));
    pm.add("exists", Prompt{"Test"});
    assert(pm.has("exists"));
    assert(!pm.has("still_nonexistent"));
    std::cout << "  [PASS]\n";
}

void test_manager_multiple_prompts()
{
    std::cout << "test_manager_multiple_prompts...\n";
    PromptManager pm;
    pm.add("greeting", Prompt{"Hello {name}!"});
    pm.add("farewell", Prompt{"Goodbye {name}!"});
    pm.add("question", Prompt{"How is {name}?"});

    assert(pm.has("greeting"));
    assert(pm.has("farewell"));
    assert(pm.has("question"));

    assert(pm.get("greeting").render({{"name", "X"}}) == "Hello X!");
    assert(pm.get("farewell").render({{"name", "Y"}}) == "Goodbye Y!");
    assert(pm.get("question").render({{"name", "Z"}}) == "How is Z?");
    std::cout << "  [PASS]\n";
}

void test_manager_list()
{
    std::cout << "test_manager_list...\n";
    PromptManager pm;
    pm.add("a", Prompt{"A"});
    pm.add("b", Prompt{"B"});
    pm.add("c", Prompt{"C"});

    auto list = pm.list();
    assert(list.size() == 3);
    std::cout << "  [PASS]\n";
}

void test_manager_list_empty()
{
    std::cout << "test_manager_list_empty...\n";
    PromptManager pm;
    auto list = pm.list();
    assert(list.size() == 0);
    std::cout << "  [PASS]\n";
}

void test_manager_overwrite()
{
    std::cout << "test_manager_overwrite...\n";
    PromptManager pm;
    pm.add("test", Prompt{"Original: {x}"});
    pm.add("test", Prompt{"Updated: {x}"});

    auto out = pm.get("test").render({{"x", "value"}});
    assert(out == "Updated: value");
    std::cout << "  [PASS]\n";
}

void test_manager_get_nonexistent()
{
    std::cout << "test_manager_get_nonexistent...\n";
    PromptManager pm;
    bool threw = false;
    try
    {
        pm.get("nonexistent");
    }
    catch (const NotFoundError&)
    {
        threw = true;
    }
    assert(threw);
    std::cout << "  [PASS]\n";
}

// ============================================================================
// TestPromptEdgeCases - Edge cases
// ============================================================================

void test_default_constructor()
{
    std::cout << "test_default_constructor...\n";
    Prompt p;
    assert(p.template_string() == "");
    auto out = p.render({});
    assert(out == "");
    std::cout << "  [PASS]\n";
}

void test_braces_in_output()
{
    std::cout << "test_braces_in_output...\n";
    // If we put actual braces in the value, they should be preserved
    Prompt p{"Output: {value}"};
    auto out = p.render({{"value", "{literal_braces}"}});
    assert(out == "Output: {literal_braces}");
    std::cout << "  [PASS]\n";
}

void test_long_template()
{
    std::cout << "test_long_template...\n";
    std::string long_text = "The quick brown fox jumps over the lazy dog. ";
    std::string tmpl;
    for (int i = 0; i < 100; ++i)
        tmpl += long_text;
    tmpl += "{var}";
    Prompt p{tmpl};
    auto out = p.render({{"var", "END"}});
    // 100 * ~45 chars = ~4500 chars + "END" = ~4503
    assert(out.length() > 4500);
    assert(out.substr(out.length() - 3) == "END");
    std::cout << "  [PASS]\n";
}

void test_unicode_in_template()
{
    std::cout << "test_unicode_in_template...\n";
    Prompt p{u8"Привет {name}! 你好!"};
    auto out = p.render({{"name", u8"мир"}});
    assert(out == u8"Привет мир! 你好!");
    std::cout << "  [PASS]\n";
}

void test_unicode_in_value()
{
    std::cout << "test_unicode_in_value...\n";
    Prompt p{"Message: {msg}"};
    auto out = p.render({{"msg", u8"日本語テスト"}});
    assert(out == u8"Message: 日本語テスト");
    std::cout << "  [PASS]\n";
}

// ============================================================================
// TestClientPromptTypes - Client-side prompt types
// ============================================================================

void test_prompt_argument_fields()
{
    std::cout << "test_prompt_argument_fields...\n";
    client::PromptArgument arg;
    arg.name = "message";
    arg.description = "The message to process";
    arg.required = true;

    assert(arg.name == "message");
    assert(arg.description.value() == "The message to process");
    assert(arg.required == true);
    std::cout << "  [PASS]\n";
}

void test_prompt_argument_optional_desc()
{
    std::cout << "test_prompt_argument_optional_desc...\n";
    client::PromptArgument arg;
    arg.name = "optional_arg";
    arg.required = false;

    assert(!arg.description.has_value());
    assert(arg.required == false);
    std::cout << "  [PASS]\n";
}

void test_prompt_info_serialization()
{
    std::cout << "test_prompt_info_serialization...\n";

    client::PromptInfo info;
    info.name = "greeting_prompt";
    info.description = "A prompt that greets the user";
    info.arguments = std::vector<client::PromptArgument>{};

    client::PromptArgument arg1;
    arg1.name = "name";
    arg1.description = "User's name";
    arg1.required = true;
    info.arguments->push_back(arg1);

    client::PromptArgument arg2;
    arg2.name = "formal";
    arg2.required = false;
    info.arguments->push_back(arg2);

    // Serialize to JSON
    Json j;
    to_json(j, info);

    assert(j["name"] == "greeting_prompt");
    assert(j["description"] == "A prompt that greets the user");
    assert(j["arguments"].size() == 2);
    assert(j["arguments"][0]["name"] == "name");
    assert(j["arguments"][0]["required"] == true);
    assert(j["arguments"][1]["required"] == false);

    // Deserialize back
    client::PromptInfo parsed;
    from_json(j, parsed);

    assert(parsed.name == info.name);
    assert(parsed.description == info.description);
    assert(parsed.arguments->size() == 2);
    assert(parsed.arguments->at(0).required == true);

    std::cout << "  [PASS]\n";
}

void test_prompt_info_minimal()
{
    std::cout << "test_prompt_info_minimal...\n";

    Json j = {{"name", "simple_prompt"}};

    client::PromptInfo info;
    from_json(j, info);

    assert(info.name == "simple_prompt");
    assert(!info.description.has_value());
    assert(!info.arguments.has_value());

    std::cout << "  [PASS]\n";
}

void test_prompt_message_user_role()
{
    std::cout << "test_prompt_message_user_role...\n";

    client::PromptMessage msg;
    msg.role = client::Role::User;

    client::TextContent text;
    text.text = "Hello, this is the user.";
    msg.content.push_back(text);

    assert(msg.role == client::Role::User);
    assert(msg.content.size() == 1);
    assert(std::holds_alternative<client::TextContent>(msg.content[0]));

    std::cout << "  [PASS]\n";
}

void test_prompt_message_assistant_role()
{
    std::cout << "test_prompt_message_assistant_role...\n";

    client::PromptMessage msg;
    msg.role = client::Role::Assistant;

    client::TextContent text;
    text.text = "I am the assistant response.";
    msg.content.push_back(text);

    assert(msg.role == client::Role::Assistant);

    std::cout << "  [PASS]\n";
}

void test_prompt_message_mixed_content()
{
    std::cout << "test_prompt_message_mixed_content...\n";

    client::PromptMessage msg;
    msg.role = client::Role::User;

    // Add text content
    client::TextContent text;
    text.text = "Here is an image:";
    msg.content.push_back(text);

    // Add image content
    client::ImageContent img;
    img.data = "iVBORw0KGgo="; // Partial PNG base64
    img.mimeType = "image/png";
    msg.content.push_back(img);

    assert(msg.content.size() == 2);
    assert(std::holds_alternative<client::TextContent>(msg.content[0]));
    assert(std::holds_alternative<client::ImageContent>(msg.content[1]));

    std::cout << "  [PASS]\n";
}

void test_list_prompts_result()
{
    std::cout << "test_list_prompts_result...\n";

    client::ListPromptsResult result;

    client::PromptInfo p1;
    p1.name = "prompt1";
    p1.description = "First prompt";

    client::PromptInfo p2;
    p2.name = "prompt2";

    result.prompts.push_back(p1);
    result.prompts.push_back(p2);
    result.nextCursor = "cursor_xyz";

    assert(result.prompts.size() == 2);
    assert(result.prompts[0].name == "prompt1");
    assert(result.prompts[1].name == "prompt2");
    assert(result.nextCursor.value() == "cursor_xyz");

    std::cout << "  [PASS]\n";
}

void test_list_prompts_result_empty()
{
    std::cout << "test_list_prompts_result_empty...\n";

    client::ListPromptsResult result;

    assert(result.prompts.empty());
    assert(!result.nextCursor.has_value());
    assert(!result._meta.has_value());

    std::cout << "  [PASS]\n";
}

void test_get_prompt_result()
{
    std::cout << "test_get_prompt_result...\n";

    client::GetPromptResult result;
    result.description = "A greeting prompt";

    // Add a user message
    client::PromptMessage user_msg;
    user_msg.role = client::Role::User;
    client::TextContent user_text;
    user_text.text = "Please greet me.";
    user_msg.content.push_back(user_text);
    result.messages.push_back(user_msg);

    // Add an assistant message
    client::PromptMessage assistant_msg;
    assistant_msg.role = client::Role::Assistant;
    client::TextContent assistant_text;
    assistant_text.text = "Hello! How can I help you today?";
    assistant_msg.content.push_back(assistant_text);
    result.messages.push_back(assistant_msg);

    assert(result.description.value() == "A greeting prompt");
    assert(result.messages.size() == 2);
    assert(result.messages[0].role == client::Role::User);
    assert(result.messages[1].role == client::Role::Assistant);

    std::cout << "  [PASS]\n";
}

void test_get_prompt_result_with_meta()
{
    std::cout << "test_get_prompt_result_with_meta...\n";

    client::GetPromptResult result;
    result._meta = Json{{"version", "1.0"}, {"author", "system"}};

    assert(result._meta.has_value());
    assert(result._meta.value()["version"] == "1.0");

    std::cout << "  [PASS]\n";
}

void test_prompt_with_embedded_resource()
{
    std::cout << "test_prompt_with_embedded_resource...\n";

    client::PromptMessage msg;
    msg.role = client::Role::User;

    client::TextContent intro;
    intro.text = "Please analyze this document:";
    msg.content.push_back(intro);

    client::EmbeddedResourceContent resource;
    resource.uri = "file:///docs/analysis.txt";
    resource.text = "Content of the document for analysis...";
    msg.content.push_back(resource);

    assert(msg.content.size() == 2);
    assert(std::holds_alternative<client::EmbeddedResourceContent>(msg.content[1]));

    auto& res = std::get<client::EmbeddedResourceContent>(msg.content[1]);
    assert(res.uri == "file:///docs/analysis.txt");

    std::cout << "  [PASS]\n";
}

void test_multiple_prompt_arguments()
{
    std::cout << "test_multiple_prompt_arguments...\n";

    client::PromptInfo info;
    info.name = "complex_prompt";
    info.arguments = std::vector<client::PromptArgument>{};

    std::vector<std::string> arg_names = {"input", "format", "language", "verbose", "max_length"};

    for (size_t i = 0; i < arg_names.size(); ++i)
    {
        client::PromptArgument arg;
        arg.name = arg_names[i];
        arg.required = (i < 2); // First two are required
        info.arguments->push_back(arg);
    }

    assert(info.arguments->size() == 5);
    assert(info.arguments->at(0).required == true);
    assert(info.arguments->at(1).required == true);
    assert(info.arguments->at(2).required == false);

    std::cout << "  [PASS]\n";
}

void test_prompt_content_parsing()
{
    std::cout << "test_prompt_content_parsing...\n";

    // Test parsing text content
    Json text_json = {{"type", "text"}, {"text", "Hello world"}};
    auto text_block = client::parse_content_block(text_json);
    assert(std::holds_alternative<client::TextContent>(text_block));
    assert(std::get<client::TextContent>(text_block).text == "Hello world");

    // Test parsing image content
    Json img_json = {{"type", "image"}, {"data", "base64data"}, {"mimeType", "image/jpeg"}};
    auto img_block = client::parse_content_block(img_json);
    assert(std::holds_alternative<client::ImageContent>(img_block));
    assert(std::get<client::ImageContent>(img_block).mimeType == "image/jpeg");

    std::cout << "  [PASS]\n";
}

void test_prompt_pagination()
{
    std::cout << "test_prompt_pagination...\n";

    // Simulating paginated prompt list
    client::ListPromptsResult page1;
    for (int i = 0; i < 10; ++i)
    {
        client::PromptInfo p;
        p.name = "prompt_" + std::to_string(i);
        page1.prompts.push_back(p);
    }
    page1.nextCursor = "page_2";

    assert(page1.prompts.size() == 10);
    assert(page1.nextCursor.has_value());

    // Last page has no cursor
    client::ListPromptsResult last_page;
    last_page.prompts.push_back(client::PromptInfo{});
    last_page.prompts[0].name = "final_prompt";

    assert(!last_page.nextCursor.has_value());

    std::cout << "  [PASS]\n";
}

int main()
{
    std::cout << "=== TestPromptRender ===\n";
    test_basic_template();
    test_template_string();
    test_multiple_variables();
    test_repeated_variable();
    test_no_variables();
    test_empty_template();
    test_only_variable();
    test_empty_variable_value();
    test_numeric_values();
    test_special_characters_in_value();
    test_json_in_value();
    test_multiline_template();
    test_adjacent_variables();

    std::cout << "\n=== TestPromptManager ===\n";
    test_manager_add_and_get();
    test_manager_has();
    test_manager_multiple_prompts();
    test_manager_list();
    test_manager_list_empty();
    test_manager_overwrite();
    test_manager_get_nonexistent();

    std::cout << "\n=== TestPromptEdgeCases ===\n";
    test_default_constructor();
    test_braces_in_output();
    test_long_template();
    test_unicode_in_template();
    test_unicode_in_value();

    std::cout << "\n=== TestClientPromptTypes ===\n";
    test_prompt_argument_fields();
    test_prompt_argument_optional_desc();
    test_prompt_info_serialization();
    test_prompt_info_minimal();
    test_prompt_message_user_role();
    test_prompt_message_assistant_role();
    test_prompt_message_mixed_content();
    test_list_prompts_result();
    test_list_prompts_result_empty();
    test_get_prompt_result();
    test_get_prompt_result_with_meta();
    test_prompt_with_embedded_resource();
    test_multiple_prompt_arguments();
    test_prompt_content_parsing();
    test_prompt_pagination();

    std::cout << "\n[OK] All prompts tests passed! (40 tests)\n";
    return 0;
}
