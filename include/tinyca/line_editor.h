// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tinyca
{

// Input editing for the REPL, split into two pure pieces so they can be tested
// without a terminal: a decoder that turns bytes into key events, and a buffer
// that applies those events to the line being typed. The terminal half (raw
// mode, redraw, completion) stays in the CLI.
//
// No readline: SPEC.md §12.1 rules out the dependency, so what is implemented
// here is the subset a REPL actually needs.

enum class Key
{
    None,
    Char,
    Enter,
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    Up,
    Down,
    Tab,
    KillToStart,  // Ctrl+U
    KillToEnd,    // Ctrl+K
    KillWordBack, // Ctrl+W
    Eof,          // Ctrl+D on an empty line
    Unknown       // a recognised-but-unhandled escape sequence, swallowed
};

struct KeyEvent
{
    Key key = Key::None;
    char ch = 0; // only meaningful for Key::Char
};

// Feeds bytes one at a time and reports a key when one is complete. Escape
// sequences arrive as several bytes (ESC [ D for Left), and a terminal can
// split them across reads, so the decoder is a small state machine rather than
// a lookup: it holds the partial sequence until it can say what it was.
//
// Both encodings of Home/End are accepted. Terminals disagree: xterm sends
// ESC[H / ESC[F, while the linux console and many emulators send ESC[1~ /
// ESC[4~ -- which is what fn+Home and fn+End produce on a keyboard without
// those keys.
class KeyDecoder
{
  public:
    // Returns true when `out` holds a complete key; false while a sequence is
    // still being collected.
    bool feed(char byte, KeyEvent &out);

  private:
    enum class State
    {
        Ground,
        Escape,  // saw ESC
        Bracket, // saw ESC [
    };

    State state_ = State::Ground;
    std::string params_; // digits collected inside a CSI sequence
};

// The line being typed, with the cursor inside it. Every operation is a pure
// edit of (text, cursor); nothing here touches the terminal.
//
// `cursor` is a BYTE offset in [0, text.size()] and every operation keeps it
// in range, so the edges are no-ops rather than errors. Callers may set it
// directly (the tests do); out of range is undefined.
struct EditBuffer
{
    std::string text;
    size_t cursor = 0;

    void insert(char c);
    void backspace(); // delete before the cursor
    void erase_at();  // delete under the cursor
    void move_left();
    void move_right();
    void move_home();
    void move_end();
    void kill_to_start();
    void kill_to_end();
    void kill_word_back();              // Ctrl+W, as in bash: the word before the cursor
    void set(const std::string &value); // replaces text, cursor to the end
    void clear();
};

// Previously entered lines, walked with Up/Down. The line being typed when the
// walk starts is kept as a draft, so walking away from it and back does not
// lose it -- the thing that makes history navigation safe to try.
class InputHistory
{
  public:
    // Blank lines and an immediate repeat of the previous entry are not
    // stored: both only make the history harder to walk.
    void add(const std::string &line);

    // Replaces `buffer` with the previous/next entry. Returns false when there
    // is nothing to move to, leaving the buffer untouched.
    bool previous(EditBuffer &buffer);
    bool next(EditBuffer &buffer);

    // Call when a line is submitted or abandoned: ends any walk in progress.
    void reset_position();

    size_t size() const
    {
        return entries_.size();
    }

  private:
    std::vector<std::string> entries_;
    size_t position_ = 0; // entries_.size() means "not walking"
    std::string draft_;
    bool draft_valid_ = false;
};

// Number of columns a string occupies: its length minus any ANSI escape
// sequences, which move no cursor. Needed to place the cursor after a prompt
// that carries colour.
//
// AUDIT NOTE: columns are counted as BYTES. For the prompt (ASCII plus escapes)
// that is exact. For a line containing multi-byte UTF-8 the count is too high,
// so the cursor is placed too far right and one Backspace removes one byte of a
// character rather than the character -- the same byte-oriented limitation
// EditBuffer has. Tasks are typed in ASCII in practice; fixing it means a
// UTF-8-aware buffer, which is not implemented.
size_t display_width(const std::string &text);

} // namespace tinyca
