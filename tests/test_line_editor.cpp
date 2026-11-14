// BSD 3-Clause License — see LICENSE at the repository root.
// REPL input editing (SPEC.md §12.1): decoding keys from bytes, editing the
// line, and walking previous entries. Split out of the CLI precisely so this
// can be tested without a terminal.
#include "tinyca/line_editor.h"

#include "test_util.h"

namespace
{

using namespace tinyca;

// Feeds a byte sequence and returns the keys it decodes to. An escape sequence
// spans several bytes, so most inputs produce fewer keys than bytes.
std::vector<KeyEvent> decode(const std::string& bytes)
{
    KeyDecoder decoder;
    std::vector<KeyEvent> keys;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        KeyEvent event;
        if (decoder.feed(bytes[i], event))
        {
            keys.push_back(event);
        }
    }
    return keys;
}

void test_printable_and_control_bytes()
{
    std::vector<KeyEvent> keys = decode("hi");
    TINYCA_CHECK_EQ(keys.size(), static_cast<size_t>(2));
    TINYCA_CHECK(keys[0].key == Key::Char);
    TINYCA_CHECK_EQ(keys[0].ch, 'h');

    TINYCA_CHECK(decode("\r")[0].key == Key::Enter);
    TINYCA_CHECK(decode("\n")[0].key == Key::Enter);
    TINYCA_CHECK(decode("\x7f")[0].key == Key::Backspace);
    TINYCA_CHECK(decode("\t")[0].key == Key::Tab);
    TINYCA_CHECK(decode("\x01")[0].key == Key::Home); // Ctrl+A
    TINYCA_CHECK(decode("\x05")[0].key == Key::End);  // Ctrl+E
    TINYCA_CHECK(decode("\x15")[0].key == Key::KillToStart);
    TINYCA_CHECK(decode("\x17")[0].key == Key::KillWordBack); // Ctrl+W
    TINYCA_CHECK(decode("\x0b")[0].key == Key::KillToEnd);
    TINYCA_CHECK(decode("\x04")[0].key == Key::Eof);
}

void test_arrow_and_navigation_sequences()
{
    TINYCA_CHECK(decode("\x1b[D")[0].key == Key::Left);
    TINYCA_CHECK(decode("\x1b[C")[0].key == Key::Right);
    TINYCA_CHECK(decode("\x1b[A")[0].key == Key::Up);
    TINYCA_CHECK(decode("\x1b[B")[0].key == Key::Down);

    // Both encodings of Home/End: terminals disagree, and the numeric forms
    // are what fn+Home / fn+End send on keyboards without those keys.
    TINYCA_CHECK(decode("\x1b[H")[0].key == Key::Home);
    TINYCA_CHECK(decode("\x1b[F")[0].key == Key::End);
    TINYCA_CHECK(decode("\x1b[1~")[0].key == Key::Home);
    TINYCA_CHECK(decode("\x1b[7~")[0].key == Key::Home);
    TINYCA_CHECK(decode("\x1b[4~")[0].key == Key::End);
    TINYCA_CHECK(decode("\x1b[8~")[0].key == Key::End);
    TINYCA_CHECK(decode("\x1b[3~")[0].key == Key::Delete);

    // The SS3 form (ESC O A) that some terminals send in application mode.
    TINYCA_CHECK(decode("\x1bOA")[0].key == Key::Up);

    // A MODIFIED arrow carries parameters: ESC[1;5C is Ctrl+Right, ESC[1;2D is
    // Shift+Left. The ';' has to be accepted as a parameter byte -- treating
    // it as the final byte ended the sequence early and typed the remaining
    // "5C" / "2D" into the line. A modifier does not change the meaning here.
    TINYCA_CHECK(decode("\x1b[1;5C")[0].key == Key::Right);
    TINYCA_CHECK_EQ(decode("\x1b[1;5C").size(), static_cast<size_t>(1));
    TINYCA_CHECK(decode("\x1b[1;2D")[0].key == Key::Left);
    TINYCA_CHECK_EQ(decode("\x1b[1;2D").size(), static_cast<size_t>(1));
    TINYCA_CHECK(decode("\x1b[1;3A")[0].key == Key::Up);
    TINYCA_CHECK(decode("\x1b[1;5F")[0].key == Key::End);
    TINYCA_CHECK(decode("\x1b[3;5~")[0].key == Key::Delete);

    // A sequence must not leak its bytes as typed characters: "ESC [ D" is one
    // key, not "[" followed by "D".
    TINYCA_CHECK_EQ(decode("\x1b[D").size(), static_cast<size_t>(1));
    // An unrecognised CSI is swallowed whole rather than half-typed.
    std::vector<KeyEvent> unknown = decode("\x1b[2J");
    TINYCA_CHECK_EQ(unknown.size(), static_cast<size_t>(1));
    TINYCA_CHECK(unknown[0].key == Key::Unknown);
}

void test_edit_buffer_insert_and_delete()
{
    EditBuffer b;
    for (char c : std::string("helo"))
    {
        b.insert(c);
    }
    TINYCA_CHECK_EQ(b.text, std::string("helo"));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(4));

    // Insert in the middle: the whole point of cursor movement.
    b.move_left();
    b.insert('l');
    TINYCA_CHECK_EQ(b.text, std::string("hello"));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(4));

    b.erase_at(); // deletes the 'o' under the cursor
    TINYCA_CHECK_EQ(b.text, std::string("hell"));
    b.backspace();
    TINYCA_CHECK_EQ(b.text, std::string("hel"));

    // Both edges are no-ops, not crashes or wrap-arounds.
    b.move_home();
    b.backspace();
    TINYCA_CHECK_EQ(b.text, std::string("hel"));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(0));
    b.move_end();
    b.erase_at();
    TINYCA_CHECK_EQ(b.text, std::string("hel"));
    b.move_right();
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(3));
    b.move_left();
    b.move_left();
    b.move_left();
    b.move_left();
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(0));
}

void test_edit_buffer_kills()
{
    EditBuffer b;
    b.set("some long line");
    b.cursor = 5;
    b.kill_to_end();
    TINYCA_CHECK_EQ(b.text, std::string("some "));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(5));

    b.set("some long line");
    b.cursor = 5;
    b.kill_to_start();
    TINYCA_CHECK_EQ(b.text, std::string("long line"));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(0));
}

// Ctrl+W, matching bash: trailing spaces go with the word, so one press after
// "gcc -o out main.c " removes "main.c" rather than just the space.
void test_kill_word_back()
{
    EditBuffer b;
    b.set("gcc -o out main.c ");
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string("gcc -o out "));
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string("gcc -o "));
    TINYCA_CHECK_EQ(b.cursor, b.text.size());

    // Mid-line: only what is before the cursor is touched. The space that
    // followed the killed word stays, exactly as in bash.
    b.set("one two three");
    b.cursor = 7; // just after "two"
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string("one  three"));
    TINYCA_CHECK_EQ(b.cursor, static_cast<size_t>(4));

    // At the start, and on an empty line: no-ops, not crashes.
    b.move_home();
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string("one  three"));
    b.clear();
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string(""));

    // All spaces: the whole run goes, leaving an empty line.
    b.set("   ");
    b.kill_word_back();
    TINYCA_CHECK_EQ(b.text, std::string(""));
}

void test_history_walk_preserves_the_draft()
{
    InputHistory history;
    history.add("first task");
    history.add("second task");

    EditBuffer b;
    b.set("half-typed");

    // Up walks back, newest first.
    TINYCA_CHECK(history.previous(b));
    TINYCA_CHECK_EQ(b.text, std::string("second task"));
    TINYCA_CHECK_EQ(b.cursor, b.text.size());
    TINYCA_CHECK(history.previous(b));
    TINYCA_CHECK_EQ(b.text, std::string("first task"));

    // Past the oldest entry there is nothing to move to, and the buffer must
    // not be cleared by trying.
    TINYCA_CHECK(!history.previous(b));
    TINYCA_CHECK_EQ(b.text, std::string("first task"));

    // Down walks forward and, at the end, restores what was being typed --
    // without that, walking history destroys an unfinished line.
    TINYCA_CHECK(history.next(b));
    TINYCA_CHECK_EQ(b.text, std::string("second task"));
    TINYCA_CHECK(history.next(b));
    TINYCA_CHECK_EQ(b.text, std::string("half-typed"));
    TINYCA_CHECK(!history.next(b));
}

void test_history_ignores_blanks_and_repeats()
{
    InputHistory history;
    history.add("ls");
    history.add("ls"); // immediate repeat
    history.add("");   // blank
    TINYCA_CHECK_EQ(history.size(), static_cast<size_t>(1));

    history.add("pwd");
    TINYCA_CHECK_EQ(history.size(), static_cast<size_t>(2));

    // Submitting ends any walk in progress: Up starts again from the newest.
    EditBuffer b;
    TINYCA_CHECK(history.previous(b));
    TINYCA_CHECK_EQ(b.text, std::string("pwd"));
}

// The prompt carries colour, and escape sequences occupy no columns. Counting
// them would push the cursor right by the length of the escape on every
// redraw.
// Seeded history comes from a transcript file, not from the keyboard, so it is
// the one path by which a control byte could reach the line buffer -- and
// recalling such a line would echo the escape straight to the terminal.
void test_history_rejects_control_bytes()
{
    InputHistory history;
    history.add("ls -la");
    history.add("rm \x1b[2Jevil");
    history.add("two\nlines");
    TINYCA_CHECK_EQ(history.size(), static_cast<size_t>(1));

    EditBuffer b;
    TINYCA_CHECK(history.previous(b));
    TINYCA_CHECK_EQ(b.text, std::string("ls -la"));
}

void test_display_width_ignores_escapes()
{
    TINYCA_CHECK_EQ(display_width("> "), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(display_width("\x1b[1;36m> \x1b[0m"), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(display_width(""), static_cast<size_t>(0));
}

void run_all_tests()
{
    TINYCA_RUN(test_printable_and_control_bytes);
    TINYCA_RUN(test_arrow_and_navigation_sequences);
    TINYCA_RUN(test_edit_buffer_insert_and_delete);
    TINYCA_RUN(test_edit_buffer_kills);
    TINYCA_RUN(test_kill_word_back);
    TINYCA_RUN(test_history_walk_preserves_the_draft);
    TINYCA_RUN(test_history_ignores_blanks_and_repeats);
    TINYCA_RUN(test_history_rejects_control_bytes);
    TINYCA_RUN(test_display_width_ignores_escapes);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
