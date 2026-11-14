// BSD 3-Clause License — see LICENSE at the repository root.
// Terminal styling (SPEC.md §15): spec parsing, the on/off switch, and the
// escape stripping that keeps a command's own colours from punching holes in
// a styled band.
#include "tinyca/appearance.h"

#include "test_util.h"

namespace
{

using namespace tinyca;

std::string ansi(const std::string& spec)
{
    bool bg = false;
    std::string err;
    return ansi_from_spec(spec, bg, err);
}

void test_basic_colours_and_attributes()
{
    // Foreground 30+i, background 40+i, bright forms 90+/100+.
    TINYCA_CHECK_EQ(ansi("red"), std::string("\x1b[31m"));
    TINYCA_CHECK_EQ(ansi("green"), std::string("\x1b[32m"));
    TINYCA_CHECK_EQ(ansi(",blue"), std::string("\x1b[44m"));
    TINYCA_CHECK_EQ(ansi("bright_black"), std::string("\x1b[90m"));
    TINYCA_CHECK_EQ(ansi(",bright_black"), std::string("\x1b[100m"));

    // Attribute first, so "bold yellow on grey" reads as 1;33;100.
    TINYCA_CHECK_EQ(ansi("yellow,bright_black,bold"), std::string("\x1b[1;33;100m"));
    TINYCA_CHECK_EQ(ansi("white,,dim"), std::string("\x1b[2;37m"));
    TINYCA_CHECK_EQ(ansi("cyan,,bold"), std::string("\x1b[1;36m"));

    // A 0-255 index uses the 256-colour form, which is NOT interchangeable
    // with a name even when it denotes the same colour.
    TINYCA_CHECK_EQ(ansi("245"), std::string("\x1b[38;5;245m"));
    TINYCA_CHECK_EQ(ansi(",19"), std::string("\x1b[48;5;19m"));

    // Whitespace around fields is the user's, not ours.
    TINYCA_CHECK_EQ(ansi(" yellow , blue , bold "), std::string("\x1b[1;33;44m"));

    // Empty means unstyled, which is how an element is switched off.
    TINYCA_CHECK_EQ(ansi(""), std::string());
    TINYCA_CHECK_EQ(ansi("  "), std::string());
    TINYCA_CHECK_EQ(ansi(",,"), std::string());
}

// A typo must not stop a run: the element is reported and left unstyled.
void test_bad_specs_are_reported_not_fatal()
{
    bool bg = false;
    std::string err;

    TINYCA_CHECK_EQ(ansi_from_spec("sparkly", bg, err), std::string());
    TINYCA_CHECK(err.find("unknown colour 'sparkly'") != std::string::npos);

    err.clear();
    TINYCA_CHECK_EQ(ansi_from_spec("red,blue,flashing", bg, err), std::string());
    TINYCA_CHECK(err.find("unknown attribute 'flashing'") != std::string::npos);

    err.clear();
    TINYCA_CHECK_EQ(ansi_from_spec("red,blue,bold,extra", bg, err), std::string());
    TINYCA_CHECK(err.find("expected fg[,bg[,attr]]") != std::string::npos);

    err.clear();
    TINYCA_CHECK_EQ(ansi_from_spec("300", bg, err), std::string()); // outside 0-255
    TINYCA_CHECK(!err.empty());
}

// Whether a background was painted decides if a block is a band (clear to end
// of line) and whether foreign escapes have to be stripped out of it.
void test_background_detection()
{
    bool bg = false;
    std::string err;
    ansi_from_spec("yellow", bg, err);
    TINYCA_CHECK(!bg);
    ansi_from_spec("yellow,blue", bg, err);
    TINYCA_CHECK(bg);
    ansi_from_spec(",blue", bg, err);
    TINYCA_CHECK(bg);
    ansi_from_spec("yellow,,bold", bg, err);
    TINYCA_CHECK(!bg);
}

void test_resolve_honours_the_on_off_switch()
{
    Config cfg;
    std::vector<std::string> warnings;

    // never: every element empty, whatever the TTY says.
    cfg.agent_color_output = "never";
    Appearance off = resolve_appearance(cfg, true, warnings);
    TINYCA_CHECK(!off.enabled);
    TINYCA_CHECK(off.assistant.empty() && off.confirm.empty() && off.action.empty());

    // always: styled even when piped, which is what a capture wants.
    cfg.agent_color_output = "always";
    Appearance on = resolve_appearance(cfg, false, warnings);
    TINYCA_CHECK(on.enabled);
    TINYCA_CHECK(!on.assistant.empty());

    // auto: follows the terminal.
    cfg.agent_color_output = "auto";
    TINYCA_CHECK(!resolve_appearance(cfg, false, warnings).enabled);
    TINYCA_CHECK(resolve_appearance(cfg, true, warnings).enabled);
    TINYCA_CHECK(warnings.empty()); // the shipped defaults all parse
}

// The defaults must keep the agent's own actions visually distinct from the
// model's words -- that is the whole point of the section.
void test_default_appearance_separates_agent_from_model()
{
    Config cfg;
    cfg.agent_color_output = "always";
    std::vector<std::string> warnings;
    Appearance a = resolve_appearance(cfg, false, warnings);

    TINYCA_CHECK(a.think != a.assistant);          // reasoning vs reply
    TINYCA_CHECK(a.confirm != a.tool_output);      // a question vs output
    TINYCA_CHECK(a.confirm != a.action);
    TINYCA_CHECK(a.action_has_background);         // the agent acting is a band
    TINYCA_CHECK(a.tool_output_has_background);
    TINYCA_CHECK(a.confirm_has_background);
}

// Output TinyCA did not produce can carry its own escapes; inside a band one
// stray reset would clear the background for the rest of the block.
void test_strip_escapes()
{
    TINYCA_CHECK_EQ(strip_escapes("plain text"), std::string("plain text"));
    TINYCA_CHECK_EQ(strip_escapes("\x1b[31mred\x1b[0m"), std::string("red"));
    TINYCA_CHECK_EQ(strip_escapes("a\x1b[1;32mb\x1b[mc"), std::string("abc"));
    TINYCA_CHECK_EQ(strip_escapes("keep\ttabs\nand\nnewlines"), std::string("keep\ttabs\nand\nnewlines"));
    // Cursor moves and erases go too -- they would scribble over the band.
    TINYCA_CHECK_EQ(strip_escapes("x\x1b[2Ky"), std::string("xy"));
    // A truncated sequence at the end must not read past it or loop.
    TINYCA_CHECK_EQ(strip_escapes("tail\x1b["), std::string("tail"));
    TINYCA_CHECK_EQ(strip_escapes("tail\x1b"), std::string("tail"));
}

void run_all_tests()
{
    TINYCA_RUN(test_basic_colours_and_attributes);
    TINYCA_RUN(test_bad_specs_are_reported_not_fatal);
    TINYCA_RUN(test_background_detection);
    TINYCA_RUN(test_resolve_honours_the_on_off_switch);
    TINYCA_RUN(test_default_appearance_separates_agent_from_model);
    TINYCA_RUN(test_strip_escapes);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
