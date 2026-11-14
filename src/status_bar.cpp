// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/status_bar.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <iostream>
#include <string>

namespace tinyca
{

namespace
{

// Reads the terminal size. Falls back to a conventional 24x80 when the
// terminal cannot say -- a bar drawn at the wrong place is still better than
// no bar, and the next draw picks up the real size if it becomes available.
void terminal_size(int &rows, int &columns)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    {
        rows = ws.ws_row;
        columns = ws.ws_col;
        return;
    }
    rows = 24;
    columns = 80;
}

// A styled run, written so the bar's own style is restored afterwards: the
// YES/NO tokens change the foreground mid-line and the background has to
// survive the change.
std::string styled(const std::string &text, const std::string &token_style, const std::string &bar_style)
{
    if (token_style.empty())
        return text;
    // The reset is not optional: the YES/NO styles carry `bold`, and SGR
    // attributes are not cleared by setting a colour. Re-applying the bar
    // style alone left bold on, and bold black renders as grey on most
    // terminals -- which is exactly how the text after YES came out.
    return token_style + text + "\x1b[0m" + bar_style;
}

// Text from outside TinyCA, reduced to something safe to paint into a fixed
// row: escape sequences removed, then any remaining control byte dropped.
//
// AUDIT NOTE: the model name comes from the SERVER (the `model` field of its
// reply), so it is untrusted input rendered into the terminal. An escape
// sequence in it would move the cursor or repaint outside the bar, and a
// newline would push the bar off its row -- the same reason command output is
// stripped before being painted into a band (§15).
std::string one_line_plain(const std::string &text)
{
    const std::string without_escapes = strip_escapes(text);
    std::string out;
    out.reserve(without_escapes.size());
    for (size_t i = 0; i < without_escapes.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(without_escapes[i]);
        if (c >= 32 && c != 127)
            out += static_cast<char>(c);
    }
    return out;
}

} // namespace

const AgentStatusRow kStatuses[] = {
    {AgentStatus::StartingUp, "starting up"},
    {AgentStatus::WaitingForInput, "waiting for user input"},
    {AgentStatus::WaitingForModel, "waiting for LLM answer"},
    {AgentStatus::ModelReasoning, "LLM is reasoning"},
    {AgentStatus::ModelReplying, "LLM is answering"},
    {AgentStatus::AwaitingApproval, "waiting for your approval"},
    {AgentStatus::RunningCommand, "running bash command"},
};

// A status with no words is a status the bar cannot report. The old switch
// over the enum had -Wall catch that; with a table, this assert does.
static_assert(sizeof(kStatuses) / sizeof(kStatuses[0]) == static_cast<size_t>(AgentStatus::Count),
              "every AgentStatus needs a row in kStatuses");

const AgentStatusRow *agent_statuses(size_t &count)
{
    count = sizeof(kStatuses) / sizeof(kStatuses[0]);
    return kStatuses;
}

const char *status_text(AgentStatus status)
{
    size_t count = 0;
    const AgentStatusRow *rows = agent_statuses(count);
    for (size_t i = 0; i < count; ++i)
        if (rows[i].status == status)
            return rows[i].text;
    return "";
}

std::string render_status_bar(const StatusInfo &info, const Appearance &style, size_t width)
{
    const std::string bar = style.status;
    const std::string model = info.model.empty() ? std::string("model unknown") : one_line_plain(info.model);
    const std::string supported_text = info.thinking_supported == Support::Yes  ? "YES"
                                       : info.thinking_supported == Support::No ? "NO"
                                                                                : "?";
    const std::string shown_text = info.thinking_shown ? "ON" : "OFF";

    const std::string prefix = std::string(" ") + status_text(info.status) + " | ";

    // Two sets of labels. The full ones are what the bar says when there is
    // room; on a narrow terminal the full labels leave only a handful of
    // columns for the model name, which is worse than naming the fields
    // tersely -- all four facts and the YES/NO colours survive either way.
    const std::string full_a = " | Thinking supported: ";
    const std::string full_b = " | Thinking showed: ";
    const std::string short_a = " | Think: ";
    const std::string short_b = " | Show: ";
    const size_t kModelMinimum = 16; // below this a model name says nothing

    // Measured against the LONGEST status, not the current one: deciding per
    // draw made the labels flip between forms as the status changed, which
    // reads as the bar glitching.
    size_t widest_prefix = 0;
    size_t status_count = 0;
    const AgentStatusRow *rows = agent_statuses(status_count);
    for (size_t i = 0; i < status_count; ++i)
    {
        const size_t candidate = std::string(rows[i].text).size() + 4; // " <status> | "
        if (candidate > widest_prefix)
            widest_prefix = candidate;
    }

    std::string label_a = full_a;
    std::string label_b = full_b;
    const size_t full_fixed =
        widest_prefix + full_a.size() + supported_text.size() + full_b.size() + shown_text.size() + 1;
    if (full_fixed + model.size() > width && full_fixed + kModelMinimum > width)
    {
        label_a = short_a;
        label_b = short_b;
    }

    const std::string suffix = label_a + supported_text + label_b + shown_text + " ";
    // AUDIT NOTE: width, truncation and padding are all BYTE counts. The fixed
    // text is ASCII, so the only input that can make bytes differ from columns
    // is the model name -- a multi-byte name is truncated mid-character and
    // over-padded, which shows as one broken glyph and a short background, not
    // as a wrapped bar. Server model names are ASCII in practice.
    std::string shown_model = model;
    if (prefix.size() + shown_model.size() + suffix.size() > width)
    {
        // The model name is the only unbounded field, so it is what gets cut:
        // the status is the part being watched.
        const size_t fixed = prefix.size() + suffix.size();
        shown_model = (fixed + 2 > width) ? std::string() : model.substr(0, width - fixed - 1) + "~";
    }

    const std::string plain = prefix + shown_model + suffix;
    if (plain.size() > width)
    {
        // Narrower than the fixed text itself. One line that fits matters more
        // than the YES/NO colours here: a bar that wraps pushes the terminal's
        // bottom row up and leaves a stripe of itself in the middle of the
        // output.
        return bar + plain.substr(0, width) + (bar.empty() ? "" : "\x1b[0m");
    }

    const std::string supported_styled = styled(supported_text,
                                                info.thinking_supported == Support::Yes  ? style.status_yes
                                                : info.thinking_supported == Support::No ? style.status_no
                                                                                         : std::string(),
                                                bar);
    const std::string shown_styled = styled(shown_text, info.thinking_shown ? style.status_yes : style.status_no, bar);

    std::string out = bar + prefix + shown_model + label_a + supported_styled + label_b + shown_styled + " ";

    // Padded with spaces so the background covers the whole width. ESC[K also
    // clears to the end of the line, but it does not paint the background on
    // every terminal, and being visible is this bar's whole job.
    out += std::string(width - plain.size(), ' ');
    if (!bar.empty())
        out += "\x1b[0m";
    return out;
}

std::string status_bar_enable_sequence(int rows)
{
    // Order matters, and every step is load-bearing:
    //
    //  - the newline first, because the row the bar is about to claim has to be
    //    free. On a terminal whose cursor is already on the last row this
    //    scrolls the screen up by one; without it the bar is painted over the
    //    last line of output.
    //  - the cursor is then placed INSIDE the new region explicitly, never
    //    saved and restored around the change. A cursor left on the last row
    //    is outside a 1..rows-1 region, and a newline there scrolls nothing:
    //    every later write lands on the same row and overwrites the previous
    //    one, which makes the whole REPL unreadable.
    return "\n\x1b[1;" + std::to_string(rows - 1) + "r\x1b[" + std::to_string(rows - 1) + ";1H";
}

std::string status_bar_draw_sequence(int rows, const std::string &bar)
{
    // Save, paint the bottom row, restore -- so a redraw in the middle of
    // streamed output does not move the text cursor. Never nested with the
    // enable sequence: DEC save has a single slot, and an inner save stole it.
    return "\x1b"
           "7\x1b[" +
           std::to_string(rows) + ";1H\x1b[2K" + bar +
           "\x1b"
           "8";
}

std::string status_bar_disable_sequence(int rows)
{
    // Give the row back, clear it, and leave the cursor on it: a leaked scroll
    // region outlives the process, and a bar left painted on screen sits under
    // the shell prompt as if it were still live.
    return "\x1b[r\x1b[" + std::to_string(rows) + ";1H\x1b[2K";
}

StatusBar::~StatusBar()
{
    disable();
}

void StatusBar::enable(bool enabled)
{
    if (!enabled)
    {
        // Not just `enabled_ = false`: that left the scroll region set, and
        // disable() then returned early because the bar was already "off" --
        // so the obvious way to switch the bar off clipped the terminal for
        // good.
        disable();
        return;
    }
    if (enabled_)
        return; // already on; enabling twice would claim a second row

    enabled_ = true;
    terminal_size(rows_, columns_);
    if (rows_ < 3)
    {
        enabled_ = false; // not enough room to give a line away
        return;
    }

    // Order matters, and every step here is load-bearing:
    //
    //  - the newline first, because the row the bar is about to claim has to be
    //    free. On a terminal whose cursor is already on the last row this
    //    scrolls the screen up by one; without it the bar would be painted over
    //    the last line of output.
    //  - the cursor is then placed INSIDE the new region explicitly, not
    //    saved and restored around the change. A cursor left on the last row
    //    is outside a 1..rows-1 region, and a newline there scrolls nothing:
    //    every subsequent write lands on the same row and overwrites the
    //    previous one, which is what made the REPL unreadable.
    std::cout << status_bar_enable_sequence(rows_);
    region_set_ = true;
    draw(); // saves and restores the cursor itself -- hence none of that here
}

void StatusBar::set_style(const Appearance &style)
{
    style_ = style;
}

void StatusBar::set_status(AgentStatus status)
{
    if (info_.status == status)
        return;
    info_.status = status;
    draw();
}

void StatusBar::set_model(const std::string &model)
{
    if (model.empty() || info_.model == model)
        return;
    info_.model = model;
    draw();
}

void StatusBar::set_thinking_supported(Support supported)
{
    if (info_.thinking_supported == supported)
        return;
    info_.thinking_supported = supported;
    draw();
}

void StatusBar::set_thinking_shown(bool shown)
{
    if (info_.thinking_shown == shown)
        return;
    info_.thinking_shown = shown;
    draw();
}

void StatusBar::draw()
{
    if (!enabled_)
        return;

    // Re-read the size every time: a resized window moves the bottom row, and
    // this is one ioctl against a redraw that is already rare.
    int rows = rows_;
    int columns = columns_;
    terminal_size(rows, columns);
    const bool resized = (rows != rows_ && rows >= 3);
    if (resized)
        rows_ = rows;
    columns_ = columns;

    if (resized)
    {
        // A shrunken terminal can leave the cursor below the new region, where
        // output would stop scrolling, so after a resize the cursor is placed
        // rather than restored.
        std::cout << "\x1b[1;" << (rows_ - 1) << "r"
                  << "\x1b[" << rows_ << ";1H\x1b[2K" << render_status_bar(info_, style_, static_cast<size_t>(columns_))
                  << "\x1b[" << (rows_ - 1) << ";1H" << std::flush;
        return;
    }

    std::cout << status_bar_draw_sequence(rows_, render_status_bar(info_, style_, static_cast<size_t>(columns_)))
              << std::flush;
}

void StatusBar::disable()
{
    if (!enabled_)
        return;
    enabled_ = false;
    if (!region_set_)
        return;
    region_set_ = false;

    std::cout << status_bar_disable_sequence(rows_) << std::flush;
}

} // namespace tinyca
