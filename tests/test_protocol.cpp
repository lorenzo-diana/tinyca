// BSD 3-Clause License — see LICENSE at the repository root.
#include "test_util.h"
#include "tinyca/fenced_protocol.h"
#include "tinyca/hermes_protocol.h"

namespace
{

using tinyca::Action;
using tinyca::FencedProtocol;
using tinyca::HermesProtocol;
using tinyca::kParseErrorToolName;
using tinyca::Message;
using tinyca::RequestOptions;
using tinyca::ToolCall;
using tinyca::ToolResult;

// ---- FencedProtocol ----

void test_fenced_well_formed_single_action()
{
    FencedProtocol proto;
    Message m;
    m.role = "assistant";
    m.content = "Here you go:\n```bash\nls -la\n```\n";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].command, std::string("ls -la"));
    TINYCA_CHECK_EQ(out[0].tool_name, std::string("bash"));
    TINYCA_CHECK(err.empty());
}

void test_fenced_no_action_is_task_complete()
{
    FencedProtocol proto;
    Message m;
    m.content = "The task is already done, nothing to run.";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK(out.empty());
}

void test_fenced_unterminated_block_consumed_by_stop_sequence()
{
    // Generation halts at the stop sequence, so the closing ``` never arrives
    // in content -- SPEC.md §6.1: treat end-of-text as the terminator.
    FencedProtocol proto;
    Message m;
    m.content = "```bash\nls -la\n"; // no closing fence
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].command, std::string("ls -la"));
}

void test_fenced_multiple_blocks_warns_and_keeps_first()
{
    FencedProtocol proto;
    Message m;
    m.content = "```bash\nfirst\n```\nand also\n```bash\nsecond\n```\n";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].command, std::string("first"));
    TINYCA_CHECK(!err.empty());
}

void test_fenced_augment_request_sets_stop_sequences()
{
    FencedProtocol proto;
    RequestOptions opts;
    proto.augment_request(opts);
    TINYCA_CHECK_EQ(opts.stop_sequences.size(), static_cast<size_t>(2));
    TINYCA_CHECK(opts.tools_json.empty());
    TINYCA_CHECK(!proto.supports_parallel());
}

void test_fenced_format_results()
{
    FencedProtocol proto;
    std::vector<ToolResult> results;
    ToolResult r;
    r.content = "output text";
    results.push_back(r);
    auto msgs = proto.format_results(results);
    TINYCA_CHECK_EQ(msgs.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(msgs[0].role, std::string("tool"));
    TINYCA_CHECK_EQ(msgs[0].content, std::string("output text"));
}

// ---- HermesProtocol ----

void test_hermes_structured_two_parallel_calls()
{
    HermesProtocol proto;
    Message m;
    m.role = "assistant";
    ToolCall tc1;
    tc1.id = "call_1";
    tc1.name = "bash";
    tc1.arguments = R"({"command":"ls"})";
    ToolCall tc2;
    tc2.id = "call_2";
    tc2.name = "bash";
    tc2.arguments = R"({"command":"pwd"})";
    m.tool_calls = {tc1, tc2};

    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(out[0].command, std::string("ls"));
    TINYCA_CHECK_EQ(out[0].id, std::string("call_1"));
    TINYCA_CHECK_EQ(out[1].command, std::string("pwd"));
    TINYCA_CHECK(proto.supports_parallel());
}

void test_hermes_structured_malformed_arguments_is_parse_error_not_abort()
{
    HermesProtocol proto;
    Message m;
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "bash";
    tc.arguments = "{not valid json";
    m.tool_calls = {tc};

    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err)); // does not abort
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].tool_name, std::string(kParseErrorToolName));
    TINYCA_CHECK(!out[0].command.empty()); // human-readable error text
}

void test_hermes_structured_nonexistent_tool_name()
{
    HermesProtocol proto;
    Message m;
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "frobnicate";
    tc.arguments = R"({"command":"ls"})";
    m.tool_calls = {tc};

    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].tool_name, std::string(kParseErrorToolName));
}

void test_hermes_text_form_single_call()
{
    HermesProtocol proto;
    Message m;
    m.content = "<tool_call>\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}\n</tool_call>";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].command, std::string("ls"));
}

void test_hermes_text_form_unterminated_block()
{
    HermesProtocol proto;
    Message m;
    m.content = "<tool_call>\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err)); // does not abort
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].tool_name, std::string(kParseErrorToolName));
}

void test_hermes_text_form_malformed_json()
{
    HermesProtocol proto;
    Message m;
    m.content = "<tool_call>\nnot json at all\n</tool_call>";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK_EQ(out.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(out[0].tool_name, std::string(kParseErrorToolName));
}

void test_hermes_no_action_is_task_complete()
{
    HermesProtocol proto;
    Message m;
    m.content = "All done, nothing left to do.";
    std::vector<Action> out;
    std::string err;
    TINYCA_CHECK(proto.parse(m, out, err));
    TINYCA_CHECK(out.empty());
}

void test_hermes_augment_request_sets_tools_and_stop()
{
    HermesProtocol proto;
    RequestOptions opts;
    proto.augment_request(opts);
    TINYCA_CHECK_EQ(opts.stop_sequences.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(opts.stop_sequences[0], std::string("</tool_call>"));
    TINYCA_CHECK(!opts.tools_json.empty());
    TINYCA_CHECK(opts.tools_json.find("\"bash\"") != std::string::npos);
    TINYCA_CHECK(!opts.use_native_completion);
}

void test_hermes_with_grammar_switches_to_native_completion()
{
    HermesProtocol proto("root ::= \"YES\"");
    RequestOptions opts;
    proto.augment_request(opts);
    TINYCA_CHECK(opts.use_native_completion);
    TINYCA_CHECK_EQ(opts.grammar, std::string("root ::= \"YES\""));
}

void test_hermes_format_results_correlates_by_id()
{
    HermesProtocol proto;
    std::vector<ToolResult> results;
    ToolResult r;
    r.id = "call_1";
    r.content = "ls output";
    results.push_back(r);
    auto msgs = proto.format_results(results);
    TINYCA_CHECK_EQ(msgs.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(msgs[0].role, std::string("tool"));
    TINYCA_CHECK_EQ(msgs[0].tool_call_id, std::string("call_1"));
}

// The label is load-bearing, not cosmetic: an unlabelled result reads as
// unrelated text to the model, which then re-emits the same command (measured
// 2/2 on Qwen2.5-Coder-1.5B). Guards the three shapes.
void test_fenced_results_are_labelled_with_their_command()
{
    tinyca::FencedProtocol protocol;

    tinyca::ToolResult ran;
    ran.command = "ls -l | wc -l";
    ran.executed = true;
    ran.ok = true;
    ran.content = "1\n\n[exit code: 0]";

    tinyca::ToolResult blocked;
    blocked.command = "rm -rf /";
    blocked.executed = false;
    blocked.ok = false;
    blocked.content = "blocked by command filter for security reasons: 'rm' is not in the whitelist";

    tinyca::ToolResult no_command; // e.g. the truncated-reply note
    no_command.content = "error: your reply was cut off";

    std::vector<tinyca::ToolResult> results;
    results.push_back(ran);
    results.push_back(blocked);
    results.push_back(no_command);

    std::vector<tinyca::Message> messages = protocol.format_results(results);
    TINYCA_CHECK_EQ(messages.size(), static_cast<size_t>(3));

    TINYCA_CHECK_EQ(messages[0].role, std::string("tool"));
    TINYCA_CHECK_EQ(messages[0].content, std::string("Output of `ls -l | wc -l`:\n1\n\n[exit code: 0]"));

    // A command that never ran must not be reported as its output.
    TINYCA_CHECK(messages[1].content.find("was not run") != std::string::npos);
    TINYCA_CHECK(messages[1].content.find("Output of") == std::string::npos);
    TINYCA_CHECK(messages[1].content.find("rm -rf /") != std::string::npos);

    // Nothing to attribute it to, so it is passed through verbatim.
    TINYCA_CHECK_EQ(messages[2].content, std::string("error: your reply was cut off"));
}

void run_all_tests()
{
    TINYCA_RUN(test_fenced_well_formed_single_action);
    TINYCA_RUN(test_fenced_no_action_is_task_complete);
    TINYCA_RUN(test_fenced_unterminated_block_consumed_by_stop_sequence);
    TINYCA_RUN(test_fenced_multiple_blocks_warns_and_keeps_first);
    TINYCA_RUN(test_fenced_augment_request_sets_stop_sequences);
    TINYCA_RUN(test_fenced_format_results);
    TINYCA_RUN(test_hermes_structured_two_parallel_calls);
    TINYCA_RUN(test_hermes_structured_malformed_arguments_is_parse_error_not_abort);
    TINYCA_RUN(test_hermes_structured_nonexistent_tool_name);
    TINYCA_RUN(test_hermes_text_form_single_call);
    TINYCA_RUN(test_hermes_text_form_unterminated_block);
    TINYCA_RUN(test_hermes_text_form_malformed_json);
    TINYCA_RUN(test_hermes_no_action_is_task_complete);
    TINYCA_RUN(test_hermes_augment_request_sets_tools_and_stop);
    TINYCA_RUN(test_hermes_with_grammar_switches_to_native_completion);
    TINYCA_RUN(test_hermes_format_results_correlates_by_id);
    TINYCA_RUN(test_fenced_results_are_labelled_with_their_command);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
