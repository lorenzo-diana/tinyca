// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/appearance.h"

#include <unistd.h>

#include "tinyca/string_util.h"

namespace tinyca
{

namespace
{

// The eight ANSI colours, in their canonical order: the foreground code is
// 30 + index, the background 40 + index, and the bright forms 90 + / 100 +.
// Anything outside this table is either a 0-255 index or a typo.
const char *kColorNames[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};

bool base_color_index(const std::string &name, int &index)
{
    for (int i = 0; i < 8; ++i)
    {
        if (name == kColorNames[i])
        {
            index = i;
            return true;
        }
    }
    return false;
}

bool parse_index(const std::string &text, int &value)
{
    if (text.empty())
        return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    const long n = std::atol(text.c_str());
    if (n < 0 || n > 255)
        return false;
    value = static_cast<int>(n);
    return true;
}

// Appends the SGR parameters for one colour. `foreground` picks between the
// 30/90 and 40/100 ranges; a numeric index uses the 256-colour form, which is
// why a name and a number are not interchangeable in the output.
bool append_color(const std::string &name, bool foreground, std::vector<std::string> &parts, std::string &err)
{
    if (name.empty())
        return true;

    const bool bright = starts_with(name, "bright_");
    const std::string base = bright ? name.substr(7) : name;

    int index = 0;
    if (base_color_index(base, index))
    {
        const int start = foreground ? (bright ? 90 : 30) : (bright ? 100 : 40);
        parts.push_back(std::to_string(start + index));
        return true;
    }
    if (!bright && parse_index(name, index))
    {
        parts.push_back(foreground ? "38" : "48");
        parts.push_back("5");
        parts.push_back(std::to_string(index));
        return true;
    }
    err = "unknown colour '" + name + "'";
    return false;
}

bool append_attribute(const std::string &attr, std::vector<std::string> &parts, std::string &err)
{
    if (attr.empty())
        return true;
    if (attr == "bold")
        parts.push_back("1");
    else if (attr == "dim")
        parts.push_back("2");
    else if (attr == "underline")
        parts.push_back("4");
    else
    {
        err = "unknown attribute '" + attr + "'";
        return false;
    }
    return true;
}

std::string resolve_one(const std::string &spec, bool &has_background, std::vector<std::string> &warnings,
                        const std::string &key)
{
    std::string err;
    const std::string ansi = ansi_from_spec(spec, has_background, err);
    if (!err.empty())
        warnings.push_back("appearance." + key + ": " + err + " -- leaving it unstyled");
    return ansi;
}

} // namespace

std::string ansi_from_spec(const std::string &spec, bool &has_background, std::string &err)
{
    has_background = false;
    const std::string trimmed = trim(spec);
    if (trimmed.empty())
        return std::string();

    // split() rather than split_trimmed(): the positions matter, so an empty
    // field has to survive as an empty field (",red" is background-only).
    std::vector<std::string> fields = split(trimmed, ',');
    if (fields.size() > 3)
    {
        err = "expected fg[,bg[,attr]], got " + std::to_string(fields.size()) + " fields";
        return std::string();
    }
    for (auto &f : fields)
        f = trim(f);
    fields.resize(3);

    std::vector<std::string> parts;
    if (!append_attribute(fields[2], parts, err))
        return std::string();
    if (!append_color(fields[0], true, parts, err))
        return std::string();
    if (!append_color(fields[1], false, parts, err))
        return std::string();
    if (parts.empty())
        return std::string();

    has_background = !fields[1].empty();
    std::string out = "\x1b[";
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? ";" : "") + parts[i];
    return out + "m";
}

Appearance resolve_appearance(const Config &config, bool stdout_is_tty, std::vector<std::string> &warnings)
{
    Appearance a;
    if (config.agent_color_output == "always")
        a.enabled = true;
    else if (config.agent_color_output == "never")
        a.enabled = false;
    else
        a.enabled = stdout_is_tty;

    if (!a.enabled)
        return a; // every element stays empty, so every call site writes plain text

    bool ignored = false;
    a.user_input = resolve_one(config.appearance_user_input, ignored, warnings, "user_input");
    a.think = resolve_one(config.appearance_think, ignored, warnings, "think");
    a.assistant = resolve_one(config.appearance_assistant, ignored, warnings, "assistant");
    a.action = resolve_one(config.appearance_action, a.action_has_background, warnings, "action");
    a.tool_output = resolve_one(config.appearance_tool_output, a.tool_output_has_background, warnings, "tool_output");
    a.confirm = resolve_one(config.appearance_confirm, a.confirm_has_background, warnings, "confirm");
    a.warning = resolve_one(config.appearance_warning, ignored, warnings, "warning");
    a.error = resolve_one(config.appearance_error, ignored, warnings, "error");
    a.notice = resolve_one(config.appearance_notice, ignored, warnings, "notice");
    a.status = resolve_one(config.appearance_status, ignored, warnings, "status");
    a.status_yes = resolve_one(config.appearance_status_yes, ignored, warnings, "status_yes");
    a.status_no = resolve_one(config.appearance_status_no, ignored, warnings, "status_no");
    return a;
}

std::string strip_escapes(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\x1b')
        {
            out += text[i];
            continue;
        }
        // CSI sequences (ESC [ ... final byte in @-~) cover colours, cursor
        // moves and erases -- the ones that would disturb a styled block.
        // Anything else after ESC is dropped with its next byte, which is
        // enough for the two-byte sequences a command realistically emits.
        if (i + 1 < text.size() && text[i + 1] == '[')
        {
            size_t j = i + 2;
            while (j < text.size() && !(text[j] >= '@' && text[j] <= '~'))
                ++j;
            i = j;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

} // namespace tinyca
