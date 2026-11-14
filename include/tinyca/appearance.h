// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

#include "tinyca/config.h"

namespace tinyca
{

// Terminal styling (SPEC.md §15). Six things must be distinguishable at a
// glance: what the user typed, the model's reasoning, the model's reply, the
// command the agent is about to run, what that command printed, and a question
// the agent is asking. Each is one `[appearance]` key, so the operator can
// pick colours that work on their terminal instead of living with ours.
//
// Spec syntax: "fg[,bg[,attr]]". Each part may be empty to leave it alone.
// Colours are the eight basic names (black red green yellow blue magenta cyan
// white), their bright_ forms, or a 0-255 index for 256-colour terminals.
// Attributes: bold, dim, underline. An empty spec means unstyled.
struct Appearance
{
    bool enabled = false;

    std::string user_input;
    std::string think;
    std::string assistant;
    std::string action;
    std::string tool_output;
    std::string confirm;
    std::string warning;
    std::string error;
    std::string notice;
    std::string status;     // the bottom status bar
    std::string status_yes; // the YES/ON words inside it
    std::string status_no;  // the NO/OFF words inside it

    // True when that element paints a background, which is what makes a block
    // read as one band rather than coloured words. Callers use it to decide
    // whether to clear to end of line and whether to strip escapes out of text
    // they do not control (see strip_escapes).
    bool action_has_background = false;
    bool tool_output_has_background = false;
    bool confirm_has_background = false;
};

// Translates one spec into an ANSI SGR prefix (e.g. "\x1b[1;33;40m"), or an
// empty string for an empty spec. On an unusable spec `err` is set and the
// element is left unstyled -- a typo in a colour name must not stop a run.
// `has_background` reports whether the spec painted one.
std::string ansi_from_spec(const std::string &spec, bool &has_background, std::string &err);

// Resolves every element from the config, honouring agent.color_output
// (auto = only when stdout is a TTY). Problems are appended to `warnings`
// rather than being fatal.
Appearance resolve_appearance(const Config &config, bool stdout_is_tty, std::vector<std::string> &warnings);

// Removes ANSI escape sequences from text TinyCA did not produce. A command's
// own output can contain colours and resets; printed inside a styled block,
// one stray reset punches a hole in the background from there to the end. The
// transcript keeps the original bytes -- only the screen copy is cleaned.
std::string strip_escapes(const std::string &text);

} // namespace tinyca
