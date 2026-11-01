// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/ini_parser.h"

#include <fstream>

#include "tinyca/string_util.h"

namespace tinyca
{

void IniParser::parse_file(const std::string &path, std::map<std::string, std::string> &out,
                           std::vector<std::string> &warnings, bool &file_found)
{
    std::ifstream in(path.c_str());
    if (!in.is_open())
    {
        file_found = false;
        return;
    }
    file_found = true;

    std::string section;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line))
    {
        ++line_no;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;
        if (trimmed[0] == '[')
        {
            size_t close = trimmed.find(']');
            if (close == std::string::npos)
            {
                warnings.push_back(path + ":" + std::to_string(line_no) + ": malformed section header (missing ']')");
                continue;
            }
            section = trim(trimmed.substr(1, close - 1));
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            warnings.push_back(path + ":" + std::to_string(line_no) + ": malformed line (no '=')");
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (key.empty())
        {
            warnings.push_back(path + ":" + std::to_string(line_no) + ": empty key");
            continue;
        }
        const std::string full_key = section + "." + key;
        // Last-wins is kept (it is what every INI reader does), but silently
        // dropping an earlier value hides a real editing mistake -- two
        // different values for one key, one of which does nothing.
        if (out.find(full_key) != out.end())
            warnings.push_back(path + ":" + std::to_string(line_no) + ": duplicate key '" + full_key +
                               "' overrides an earlier value");
        out[full_key] = value;
    }
}

} // namespace tinyca
