// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/string_util.h"

#include <algorithm>
#include <cctype>

namespace tinyca
{

std::string trim(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string &s, char delim)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos)
        {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string to_lower(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace tinyca
