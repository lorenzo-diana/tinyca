// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "tinyca/message.h"

namespace tinyca
{

// Replaying a prior session (`--resume`). The transcript written by Logger is
// the only session record TinyCA keeps, so resuming means reading it back:
// user_task/response/tool_result lines become history, "request" lines are
// skipped (they are a derived re-encoding of history, not new state).
//
// The system prompt is a special case. It is generated at startup from
// config/system_prompt.txt plus a live scan of skills_dir, so it can differ
// from the one the stored session actually ran with -- a skill added, removed
// or renamed since then changes it. The stored prompt is therefore returned
// separately, for the caller to compare and reconcile (SPEC.md §12.2); it is
// never silently replayed as history.
struct ResumedSession
{
    std::vector<Message> history;     // user/assistant/tool turns, in order
    std::string stored_system_prompt; // from the first logged request; may be empty
    bool found_system_prompt = false; // false when no request entry carried one
    // Absolute work_dir the session last ran in, from its most recent
    // session_start / work_dir_update entry. Resuming somewhere else means
    // every relative path in the replayed history now points elsewhere, so the
    // caller reconciles this like the system prompt (SPEC.md §12.2).
    // Transcripts written before this entry existed have none, which is
    // reported rather than guessed.
    std::string stored_work_dir;
    bool found_work_dir = false;

    // Options the session changed at runtime (`/toggle_show_thinking`), keyed
    // by their config name, last value wins. The transcript already records
    // non-history state this way -- session_rename, work_dir_update,
    // system_prompt_update -- and an option the operator deliberately changed
    // mid-session is the same kind of fact: part of how the session was left,
    // not part of what was said in it.
    std::map<std::string, std::string> options;
};

// Reads `path`. Returns false only when the file cannot be opened (`err` set);
// malformed or truncated lines are skipped, matching the project's
// "diagnostics are never fatal" convention.
bool load_session(const std::string &path, ResumedSession &out, std::string &err);

// Reads one boolean option out of a loaded session. Returns false when the
// session never changed that option, leaving `value` alone, so the caller's
// own default (config file, then built-in) stands. An unparseable value is
// reported in `err` and treated as absent -- a corrupt transcript must not
// flip a setting silently.
bool resumed_bool_option(const ResumedSession &session, const std::string &key, bool &value, std::string &err);

// One-line, human-readable account of how two system prompts differ: the first
// line number that differs and both sizes. Used in the prompt shown to the
// user, so it has to be short rather than a full diff.
std::string describe_prompt_difference(const std::string &stored, const std::string &current);

} // namespace tinyca
