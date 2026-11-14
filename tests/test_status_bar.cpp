// BSD 3-Clause License — see LICENSE at the repository root.
// The bottom status bar (SPEC.md §15.1). Only the rendering is tested: that is
// where the composition, the YES/NO colouring and the width handling live, and
// it is a pure function precisely so no terminal is needed here.
#include "tinyca/status_bar.h"

#include <iostream>
#include <sstream>

#include "test_util.h"

namespace
{

using namespace tinyca;

// A style with recognisable markers instead of real escapes, so a test can say
// where the bar style and the YES/NO styles were applied.
Appearance marker_style()
{
    Appearance a;
    a.enabled = true;
    a.status = "<BAR>";
    a.status_yes = "<YES>";
    a.status_no = "<NO>";
    return a;
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

void test_every_status_has_text()
{
    // A bar that renders an empty status says nothing about what TinyCA is
    // doing, which is its entire purpose.
    size_t count = 0;
    const AgentStatusRow *rows = agent_statuses(count);
    TINYCA_CHECK(count > 0);
    for (size_t i = 0; i < count; ++i)
    {
        TINYCA_CHECK(std::string(status_text(rows[i].status)).size() > 0);
        // Via the lookup as well as the table, so a row whose status field does
        // not match its own entry cannot pass.
        TINYCA_CHECK_EQ(std::string(status_text(rows[i].status)), std::string(rows[i].text));
    }
}

void test_renders_all_four_fields()
{
    StatusInfo info;
    info.status = AgentStatus::WaitingForModel;
    info.model = "Qwen3-4B-Thinking";
    info.thinking_supported = Support::Yes;
    info.thinking_shown = true;

    const std::string bar = render_status_bar(info, marker_style(), 100);
    TINYCA_CHECK(contains(bar, "waiting for LLM answer"));
    TINYCA_CHECK(contains(bar, "Qwen3-4B-Thinking"));
    TINYCA_CHECK(contains(bar, "Thinking supported: "));
    TINYCA_CHECK(contains(bar, "Thinking showed: "));
    TINYCA_CHECK(contains(bar, " | ")); // the separator the operator asked for
}

void test_yes_and_on_are_styled_apart_from_no_and_off()
{
    StatusInfo info;
    info.model = "m";
    info.thinking_supported = Support::Yes;
    info.thinking_shown = true;
    std::string bar = render_status_bar(info, marker_style(), 100);
    TINYCA_CHECK(contains(bar, "<YES>YES"));
    TINYCA_CHECK(contains(bar, "<YES>ON"));
    TINYCA_CHECK(!contains(bar, "<NO>"));

    info.thinking_supported = Support::No;
    info.thinking_shown = false;
    bar = render_status_bar(info, marker_style(), 100);
    TINYCA_CHECK(contains(bar, "<NO>NO"));
    TINYCA_CHECK(contains(bar, "<NO>OFF"));
    TINYCA_CHECK(!contains(bar, "<YES>"));

    // A styled word ends with a reset and then the bar style again. Both
    // halves matter: without the bar style the background would stop at the
    // first YES, and without the RESET the word's `bold` would stay on for the
    // rest of the line -- bold black renders as grey, which is exactly how the
    // text after YES came out before this was fixed.
    TINYCA_CHECK(contains(bar, "<NO>NO\x1b[0m<BAR>"));
}

void test_unknown_thinking_support_is_not_guessed()
{
    StatusInfo info;
    info.model = "m";
    const std::string bar = render_status_bar(info, marker_style(), 100);
    TINYCA_CHECK(contains(bar, "Thinking supported: ?"));
    // "?" is neither good nor bad news, so it wears neither colour.
    TINYCA_CHECK(!contains(bar, "<YES>?"));
    TINYCA_CHECK(!contains(bar, "<NO>?"));
}

void test_model_is_named_as_unknown_before_the_first_reply()
{
    StatusInfo info;
    TINYCA_CHECK(contains(render_status_bar(info, marker_style(), 100), "model unknown"));
}

// The bar is one line: if it does not fit it wraps, pushing the terminal's
// bottom row up and leaving a stripe of bar in the middle of the output.
void test_width_is_never_exceeded()
{
    StatusInfo info;
    info.status = AgentStatus::AwaitingApproval;
    info.model = "a-very-long-model-name-that-will-not-fit-in-this-bar-at-all";
    info.thinking_supported = Support::Yes;

    for (size_t width = 20; width <= 120; width += 7)
    {
        const std::string plain = strip_escapes(render_status_bar(info, Appearance(), width));
        TINYCA_CHECK(plain.size() <= width);
    }

    // Wide enough for everything: full labels, and the model name is cut only
    // as far as it has to be.
    const std::string wide = render_status_bar(info, Appearance(), 120);
    TINYCA_CHECK(contains(wide, "waiting for your approval"));
    TINYCA_CHECK(contains(wide, "Thinking supported: YES"));
    TINYCA_CHECK(contains(wide, "Thinking showed: OFF"));
    TINYCA_CHECK(contains(wide, "a-very-long-model-name")); // shortened, with a marker
    TINYCA_CHECK(contains(wide, "~"));
    TINYCA_CHECK(!contains(wide, "at-all"));

    // Narrow: the full labels would leave the model name unreadable, so the
    // labels go terse instead and all four facts survive.
    const std::string narrow = render_status_bar(info, Appearance(), 80);
    TINYCA_CHECK(contains(narrow, "waiting for your approval"));
    TINYCA_CHECK(contains(narrow, "Think: YES"));
    TINYCA_CHECK(contains(narrow, "Show: OFF"));
    TINYCA_CHECK(contains(narrow, "a-very-long-model")); // more room than the full labels left
    TINYCA_CHECK(!contains(narrow, "Thinking supported"));

    // Narrower than even the terse fixed text: the status still leads the
    // line, because that is the part being watched.
    TINYCA_CHECK(contains(render_status_bar(info, Appearance(), 40), "waiting for your approval"));
}

void test_background_spans_the_whole_width()
{
    StatusInfo info;
    info.model = "m";
    const std::string plain = strip_escapes(render_status_bar(info, marker_style(), 100));
    // Markers are not escapes, so discount them before measuring the padding.
    std::string without_markers = plain;
    const char *markers[] = {"<BAR>", "<YES>", "<NO>"};
    for (size_t m = 0; m < 3; ++m)
    {
        size_t at;
        while ((at = without_markers.find(markers[m])) != std::string::npos)
            without_markers.erase(at, std::string(markers[m]).size());
    }
    TINYCA_CHECK_EQ(without_markers.size(), static_cast<size_t>(100));
}

// The terminal protocol. These three sequences caused the only serious defect
// the bar has had: the cursor was left OUTSIDE the scroll region, so a newline
// scrolled nothing and every write overwrote the previous line -- the REPL
// became unreadable while the bar itself looked perfect.
void test_enable_leaves_the_cursor_inside_the_region()
{
    const std::string seq = status_bar_enable_sequence(24);

    // A newline first, so the row the bar claims is free even when the cursor
    // is already on the last row.
    TINYCA_CHECK_EQ(seq.substr(0, 1), std::string("\n"));
    TINYCA_CHECK(contains(seq, "\x1b[1;23r"));  // region is 1..rows-1
    TINYCA_CHECK(contains(seq, "\x1b[23;1H")); // cursor placed on its last row

    // The defect in one assertion: enable must not save and restore the
    // cursor. Restoring put it back on the last row -- outside the region --
    // and the nested save inside the first draw stole the single DEC save slot.
    TINYCA_CHECK(!contains(seq, "\x1b" "7"));
    TINYCA_CHECK(!contains(seq, "\x1b" "8"));

    // The cursor must be placed after the region is set: a position set first
    // is not clamped into a region that does not exist yet.
    TINYCA_CHECK(seq.find("\x1b[1;23r") < seq.find("\x1b[23;1H"));
}

void test_draw_restores_the_text_cursor()
{
    const std::string seq = status_bar_draw_sequence(24, "BAR");
    // Save, paint row 24, restore: a redraw lands in the middle of streamed
    // output, so it must not move the cursor that output is using.
    TINYCA_CHECK_EQ(seq.substr(0, 2), std::string("\x1b" "7"));
    TINYCA_CHECK(contains(seq, "\x1b[24;1H"));
    TINYCA_CHECK(contains(seq, "\x1b[2K")); // the row is cleared before painting
    TINYCA_CHECK(contains(seq, "BAR"));
    TINYCA_CHECK_EQ(seq.substr(seq.size() - 2), std::string("\x1b" "8"));
}

void test_disable_releases_the_region()
{
    const std::string seq = status_bar_disable_sequence(24);
    TINYCA_CHECK(contains(seq, "\x1b[r"));      // full-screen region restored
    TINYCA_CHECK(contains(seq, "\x1b[24;1H")); // and the bar's row cleared
    TINYCA_CHECK(contains(seq, "\x1b[2K"));
}

// The model name is whatever the server said it was serving. Painting it raw
// would let a reply move the cursor or push the bar off its row.
void test_model_name_from_the_server_is_sanitised()
{
    StatusInfo info;
    info.model = "evil\x1b[31m\x1b[2Jmodel\nsecond line\x07";
    const std::string bar = render_status_bar(info, marker_style(), 200);
    TINYCA_CHECK(bar.find("\x1b[31m") == std::string::npos);
    TINYCA_CHECK(bar.find("\x1b[2J") == std::string::npos);
    TINYCA_CHECK(bar.find('\n') == std::string::npos);
    TINYCA_CHECK(bar.find('\x07') == std::string::npos);
    // What survives is the readable text, in order.
    TINYCA_CHECK(contains(bar, "evilmodelsecond line"));

    // And the bar is still exactly one line of `width` columns afterwards.
    TINYCA_CHECK_EQ(strip_escapes(render_status_bar(info, Appearance(), 200)).size(), static_cast<size_t>(200));
}

// ---- The StatusBar object itself (T2) -------------------------------------
//
// The terminal is not available here: stdout is not a TTY under the runner, so
// terminal_size() takes its 24x80 fallback and every sequence is predictable.
// What is checked is the state machine -- when the bar writes, when it does
// not, and that it always gives the row back.
//
// Not reachable from here, and so not covered: the `rows < 3` refusal and the
// resize branch of draw(), both of which need a controllable terminal size.

// Runs `body` with std::cout redirected, and returns what was written.
std::string captured(void (*body)(StatusBar &), StatusBar &bar)
{
    std::ostringstream buffer;
    std::streambuf *saved = std::cout.rdbuf(buffer.rdbuf());
    body(bar);
    std::cout.rdbuf(saved);
    return buffer.str();
}

void enable_on(StatusBar &bar)
{
    bar.enable(true);
}
void enable_off(StatusBar &bar)
{
    bar.enable(false);
}
void change_status(StatusBar &bar)
{
    bar.set_status(AgentStatus::RunningCommand);
}
void repeat_status(StatusBar &bar)
{
    bar.set_status(AgentStatus::RunningCommand);
}
void call_disable(StatusBar &bar)
{
    bar.disable();
}

void test_disabled_bar_writes_nothing()
{
    StatusBar bar;
    // Every method is a no-op before enable(), which is what keeps the call
    // sites free of conditionals.
    TINYCA_CHECK_EQ(captured(change_status, bar), std::string(""));
    TINYCA_CHECK_EQ(captured(call_disable, bar), std::string(""));
}

void test_enable_claims_the_row_once_and_draws()
{
    StatusBar bar;
    const std::string first = captured(enable_on, bar);
    TINYCA_CHECK(first.find("\x1b[1;23r") != std::string::npos); // 24-row fallback
    TINYCA_CHECK(first.find("\x1b[24;1H") != std::string::npos); // the bar painted
    TINYCA_CHECK(first.find("starting up") != std::string::npos);

    // Enabling twice must not claim a second row: that newline would scroll a
    // line of output away every time.
    TINYCA_CHECK_EQ(captured(enable_on, bar), std::string(""));
}

void test_only_a_real_change_redraws()
{
    StatusBar bar;
    captured(enable_on, bar);
    TINYCA_CHECK(captured(change_status, bar).find("running bash command") != std::string::npos);
    // Setting the same value again is not a change; redrawing on every call
    // would repaint the bar for every chunk of a streamed reply.
    TINYCA_CHECK_EQ(captured(repeat_status, bar), std::string(""));
}

// The defect this guards: enable(false) used to set the flag and return,
// leaving the scroll region in place -- and disable() then did nothing because
// the bar was already "off", so the terminal stayed clipped for good.
void test_enable_false_releases_the_region()
{
    StatusBar bar;
    captured(enable_on, bar);
    const std::string off = captured(enable_off, bar);
    TINYCA_CHECK(off.find("\x1b[r") != std::string::npos);      // region released
    TINYCA_CHECK(off.find("\x1b[24;1H") != std::string::npos);  // row cleared
    // And it is off afterwards: no further writes, and nothing to release twice.
    TINYCA_CHECK_EQ(captured(change_status, bar), std::string(""));
    TINYCA_CHECK_EQ(captured(call_disable, bar), std::string(""));
}

void test_destructor_releases_the_region()
{
    std::ostringstream buffer;
    std::streambuf *saved = std::cout.rdbuf(buffer.rdbuf());
    {
        StatusBar bar;
        bar.enable(true);
    }
    std::cout.rdbuf(saved);
    TINYCA_CHECK(buffer.str().find("\x1b[r") != std::string::npos);
}

void run_all_tests()
{
    TINYCA_RUN(test_every_status_has_text);
    TINYCA_RUN(test_renders_all_four_fields);
    TINYCA_RUN(test_yes_and_on_are_styled_apart_from_no_and_off);
    TINYCA_RUN(test_unknown_thinking_support_is_not_guessed);
    TINYCA_RUN(test_model_is_named_as_unknown_before_the_first_reply);
    TINYCA_RUN(test_model_name_from_the_server_is_sanitised);
    TINYCA_RUN(test_width_is_never_exceeded);
    TINYCA_RUN(test_background_spans_the_whole_width);
    TINYCA_RUN(test_enable_leaves_the_cursor_inside_the_region);
    TINYCA_RUN(test_draw_restores_the_text_cursor);
    TINYCA_RUN(test_disable_releases_the_region);
    TINYCA_RUN(test_disabled_bar_writes_nothing);
    TINYCA_RUN(test_enable_claims_the_row_once_and_draws);
    TINYCA_RUN(test_only_a_real_change_redraws);
    TINYCA_RUN(test_enable_false_releases_the_region);
    TINYCA_RUN(test_destructor_releases_the_region);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
