// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/line_editor.h"

#include "tinyca/appearance.h"

namespace tinyca
{

bool KeyDecoder::feed(char byte, KeyEvent &out)
{
    out = KeyEvent();

    switch (state_)
    {
    case State::Ground:
        if (byte == '\x1b')
        {
            state_ = State::Escape;
            params_.clear();
            return false;
        }
        if (byte == '\r' || byte == '\n')
        {
            out.key = Key::Enter;
            return true;
        }
        if (byte == 127 || byte == 8)
        {
            out.key = Key::Backspace;
            return true;
        }
        if (byte == '\t')
        {
            out.key = Key::Tab;
            return true;
        }
        if (byte == 1) // Ctrl+A
        {
            out.key = Key::Home;
            return true;
        }
        if (byte == 5) // Ctrl+E
        {
            out.key = Key::End;
            return true;
        }
        if (byte == 21) // Ctrl+U
        {
            out.key = Key::KillToStart;
            return true;
        }
        if (byte == 23) // Ctrl+W
        {
            out.key = Key::KillWordBack;
            return true;
        }
        if (byte == 11) // Ctrl+K
        {
            out.key = Key::KillToEnd;
            return true;
        }
        if (byte == 4) // Ctrl+D
        {
            out.key = Key::Eof;
            return true;
        }
        if (byte >= 32 && byte < 127)
        {
            out.key = Key::Char;
            out.ch = byte;
            return true;
        }
        return false; // other control bytes are ignored outright

    case State::Escape:
        if (byte == '[' || byte == 'O') // CSI, or the SS3 form some terminals use
        {
            state_ = State::Bracket;
            return false;
        }
        // ESC followed by anything else: drop both, rather than letting the
        // next byte act as a command the user did not type.
        state_ = State::Ground;
        return false;

    case State::Bracket:
        // Parameter bytes: digits and the ';' separator that joins them.
        // Accepting ';' is not cosmetic -- a modified arrow is ESC[1;5C
        // (Ctrl+Right) or ESC[1;2D (Shift+Left), and treating ';' as the final
        // byte ended the sequence early and left "5C" / "2D" to be typed into
        // the line as ordinary characters.
        if ((byte >= '0' && byte <= '9') || byte == ';')
        {
            params_ += byte;
            return false;
        }
        state_ = State::Ground;
        switch (byte)
        {
        // A modifier does not change what the key means here: Ctrl+Right
        // moves right. Word-wise movement is not implemented, and moving by
        // one is a better answer than refusing the key.
        case 'A':
            out.key = Key::Up;
            return true;
        case 'B':
            out.key = Key::Down;
            return true;
        case 'C':
            out.key = Key::Right;
            return true;
        case 'D':
            out.key = Key::Left;
            return true;
        case 'H':
            out.key = Key::Home;
            return true;
        case 'F':
            out.key = Key::End;
            return true;
        case '~': {
            // The numeric forms: 1/7 Home, 4/8 End, 3 Delete. fn+Home and
            // fn+End produce these on keyboards without dedicated keys.
            //
            // Only the FIRST parameter identifies the key; anything after the
            // ';' is a modifier (Ctrl+Delete is ESC[3;5~), and comparing the
            // whole parameter string made every modified form Unknown.
            const std::string key_param = params_.substr(0, params_.find(';'));
            if (key_param == "1" || key_param == "7")
                out.key = Key::Home;
            else if (key_param == "4" || key_param == "8")
                out.key = Key::End;
            else if (key_param == "3")
                out.key = Key::Delete;
            else
                out.key = Key::Unknown;
            return true;
        }
        default:
            out.key = Key::Unknown;
            return true;
        }
    }
    return false;
}

void EditBuffer::insert(char c)
{
    text.insert(text.begin() + static_cast<long>(cursor), c);
    ++cursor;
}

void EditBuffer::backspace()
{
    if (cursor == 0)
        return;
    text.erase(cursor - 1, 1);
    --cursor;
}

void EditBuffer::erase_at()
{
    if (cursor >= text.size())
        return;
    text.erase(cursor, 1);
}

void EditBuffer::move_left()
{
    if (cursor > 0)
        --cursor;
}

void EditBuffer::move_right()
{
    if (cursor < text.size())
        ++cursor;
}

void EditBuffer::move_home()
{
    cursor = 0;
}

void EditBuffer::move_end()
{
    cursor = text.size();
}

void EditBuffer::kill_to_start()
{
    text.erase(0, cursor);
    cursor = 0;
}

void EditBuffer::kill_to_end()
{
    text.erase(cursor);
}

void EditBuffer::kill_word_back()
{
    // bash's Ctrl+W: skip any spaces immediately before the cursor, then
    // delete back to the space before the word. Skipping the spaces first is
    // what makes it useful at the end of "make -j4 " -- without it the first
    // press would only eat the trailing space.
    size_t start = cursor;
    while (start > 0 && text[start - 1] == ' ')
        --start;
    while (start > 0 && text[start - 1] != ' ')
        --start;
    text.erase(start, cursor - start);
    cursor = start;
}

void EditBuffer::set(const std::string &value)
{
    text = value;
    cursor = text.size();
}

void EditBuffer::clear()
{
    text.clear();
    cursor = 0;
}

void InputHistory::add(const std::string &line)
{
    reset_position();
    if (line.empty())
        return;
    // AUDIT NOTE: lines typed at the prompt cannot contain control bytes (the
    // decoder only accepts 32..126 as text), but seeded history comes from a
    // transcript file, which is not necessarily one TinyCA wrote. Recalling
    // such a line echoes it to the terminal, so a stray escape sequence would
    // be executed by it -- the check belongs here, where both sources meet.
    for (size_t i = 0; i < line.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c < 32 || c == 127)
            return;
    }
    if (!entries_.empty() && entries_.back() == line)
        return;
    entries_.push_back(line);
    position_ = entries_.size();
}

bool InputHistory::previous(EditBuffer &buffer)
{
    if (entries_.empty() || position_ == 0)
        return false;
    if (position_ == entries_.size())
    {
        // Stepping off the line being typed: keep it, so Down comes back to it.
        draft_ = buffer.text;
        draft_valid_ = true;
    }
    --position_;
    buffer.set(entries_[position_]);
    return true;
}

bool InputHistory::next(EditBuffer &buffer)
{
    if (position_ >= entries_.size())
        return false;
    ++position_;
    if (position_ == entries_.size())
    {
        buffer.set(draft_valid_ ? draft_ : std::string());
        return true;
    }
    buffer.set(entries_[position_]);
    return true;
}

void InputHistory::reset_position()
{
    position_ = entries_.size();
    draft_.clear();
    draft_valid_ = false;
}

size_t display_width(const std::string &text)
{
    return strip_escapes(text).size();
}

} // namespace tinyca
