// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <cstddef>
#include <string>

#include "tinyca/appearance.h"

namespace tinyca
{

// A one-line bar pinned to the bottom of the terminal, answering the questions
// you otherwise have to guess at while a slow model works: what is TinyCA
// doing right now, which model is actually answering, and whether its
// reasoning exists and is being shown.
//
// Rendering and terminal control are deliberately separate: render_status_bar
// is a pure function over the state, so the composition, the YES/NO colouring
// and the width handling are testable without a terminal.

enum class AgentStatus
{
    StartingUp,
    WaitingForInput,
    WaitingForModel,  // request sent, nothing back yet
    ModelReasoning,   // reasoning tokens arriving
    ModelReplying,    // answer tokens arriving
    AwaitingApproval, // a command needs the user's decision
    RunningCommand,

    // Must stay last: the table in status_bar.cpp static_asserts its row count
    // against this, which is what makes adding a status without its text a
    // compile error. The switch this table replaced got that from -Wswitch.
    Count,
};

// Every status with the words it shows. One table, because the set was
// previously written out three times -- an array, a switch and the test -- and
// all three had to be edited in step.
struct AgentStatusRow
{
    AgentStatus status;
    const char *text;
};

const AgentStatusRow *agent_statuses(size_t &count);
const char *status_text(AgentStatus status);

// Whether the served model produces reasoning. Unknown until a turn has
// completed: a model that simply had nothing to think about on turn one is not
// evidence either way, so the bar says so rather than guessing.
enum class Support
{
    Unknown,
    Yes,
    No,
};

struct StatusInfo
{
    AgentStatus status = AgentStatus::StartingUp;
    std::string model; // as reported by the server
    Support thinking_supported = Support::Unknown;
    bool thinking_shown = false;
};

// The bar as a single line of text with styling applied, padded to `width` so
// the background spans the terminal. Over-long content is shortened by
// truncating the model name first -- it is the only unbounded field, and the
// status is the part you are watching.
std::string render_status_bar(const StatusInfo &info, const Appearance &style, size_t width);

// The terminal protocol, as three pure functions over the row count, so the
// escape sequences can be asserted in a test instead of being eyeballed on a
// screen. Getting these wrong does not look like a bar defect: it looks like
// the whole REPL has stopped scrolling.
//
// `rows` is the terminal height; the bar occupies row `rows` and the scroll
// region covers 1..rows-1.
std::string status_bar_enable_sequence(int rows);
std::string status_bar_draw_sequence(int rows, const std::string &bar);
std::string status_bar_disable_sequence(int rows);

// Owns the terminal side: reserves the bottom line with a scroll region so
// ordinary output never scrolls over the bar, redraws it on demand, and puts
// the terminal back as it was on destruction.
//
// Caller invariants, none of them checked here:
//  - every call comes from the MAIN thread. The bar writes to std::cout from
//    whichever call site changes the state -- the agent's streaming hooks, the
//    REPL, the confirm prompt -- all of which run on the main thread.
//    ControlThread only sets flags and cancels I/O (src/control.cpp); it never
//    prints, so there is no interleaving to protect against and no lock here.
//  - enable() before any set_*/draw(). Before it, every method is a no-op by
//    design, so a bar that was never enabled costs nothing at the call sites.
//
// AUDIT NOTES:
//  - There is no SIGWINCH handler. The size is re-read by ioctl on each draw,
//    so a resized window is corrected at the next STATE CHANGE, not at the
//    moment of the resize: a window resized while the model is quiet leaves
//    the bar on the old row until something moves.
//  - The scroll region is released by disable()/the destructor, i.e. only on a
//    normal return from main. A process killed outside that path (SIGKILL, or
//    a SIGTERM arriving while the REPL blocks in read()) leaves the terminal
//    with a 1..rows-1 region; `reset`, or printing ESC[r, restores it.
class StatusBar
{
  public:
    ~StatusBar();

    // No-op unless `enabled`, which keeps every call site free of
    // conditionals. The caller passes false when stdout is not a terminal or
    // appearance.status_bar is off; agent.color_output does NOT disable the
    // bar -- with colour off it is simply drawn unstyled.
    void enable(bool enabled);
    void set_style(const Appearance &style);

    void set_status(AgentStatus status);
    void set_model(const std::string &model);
    void set_thinking_supported(Support supported);
    void set_thinking_shown(bool shown);

    void draw();
    void disable();

    const StatusInfo &info() const
    {
        return info_;
    }

  private:
    bool enabled_ = false;
    bool region_set_ = false;
    int rows_ = 0;
    int columns_ = 0;
    StatusInfo info_;
    Appearance style_;
};

} // namespace tinyca
