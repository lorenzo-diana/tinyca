// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/skills.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "tinyca/string_util.h"

namespace tinyca
{

namespace
{

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads the first few lines of `path` looking for "# @name:", "# @description:",
// and "# @usage:" markers (order and exact comment prefix don't matter, just
// that the marker text appears on the line). All three are required.
bool parse_header(const std::string &path, SkillInfo &info, std::string &missing_field)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        missing_field = "file unreadable";
        return false;
    }

    std::string line;
    int lines_read = 0;
    const int max_header_lines = 20;
    while (lines_read < max_header_lines && std::getline(f, line))
    {
        ++lines_read;
        size_t pos;
        if ((pos = line.find("@name:")) != std::string::npos)
            info.name = trim(line.substr(pos + 6));
        else if ((pos = line.find("@description:")) != std::string::npos)
            info.description = trim(line.substr(pos + 14));
        else if ((pos = line.find("@usage:")) != std::string::npos)
            info.usage = trim(line.substr(pos + 7));
    }

    if (info.name.empty())
    {
        missing_field = "@name";
        return false;
    }
    if (info.description.empty())
    {
        missing_field = "@description";
        return false;
    }
    if (info.usage.empty())
    {
        missing_field = "@usage";
        return false;
    }
    return true;
}

} // namespace

bool SkillRegistry::discover(const std::string &skills_dir, std::vector<std::string> &warnings)
{
    skills_.clear();

    DIR *d = opendir(skills_dir.c_str());
    if (d == nullptr)
    {
        warnings.push_back("skills_dir not found or unreadable: " + skills_dir);
        return true;
    }

    std::vector<std::string> filenames;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        if (!ends_with(name, ".sh"))
            continue;
        filenames.push_back(name);
    }
    closedir(d);
    std::sort(filenames.begin(), filenames.end()); // deterministic Tier 1 order

    for (const auto &fname : filenames)
    {
        std::string full_path = skills_dir;
        if (!full_path.empty() && full_path.back() != '/')
            full_path += '/';
        full_path += fname;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0)
        {
            warnings.push_back(full_path + ": cannot stat, skipped");
            continue;
        }
        if ((st.st_mode & S_IXUSR) == 0)
        {
            warnings.push_back(full_path + ": not executable, skipped");
            continue;
        }

        SkillInfo info;
        info.path = full_path;
        std::string missing_field;
        if (!parse_header(full_path, info, missing_field))
        {
            warnings.push_back(full_path + ": missing " + missing_field + ", skipped");
            continue;
        }
        skills_.push_back(info);
    }
    return true;
}

std::string SkillRegistry::render_tier1(size_t max_tier1_skills) const
{
    std::string out = "Available skills (run them with the bash tool):\n";
    size_t n = std::min(skills_.size(), max_tier1_skills);
    for (size_t i = 0; i < n; ++i)
    {
        out += "- " + skills_[i].name + ": " + skills_[i].description + "\n";
        out += "  usage: " + skills_[i].usage + "\n";
    }
    return out;
}

bool SkillRegistry::load_tier2(const std::string &name, std::string &body) const
{
    for (const auto &s : skills_)
    {
        if (s.name == name)
        {
            std::ifstream f(s.path);
            if (!f.is_open())
                return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
            return true;
        }
    }
    return false;
}

std::vector<std::string> SkillRegistry::paths() const
{
    std::vector<std::string> out;
    for (const auto &s : skills_)
        out.push_back(s.path);
    return out;
}

bool SkillRegistry::find_show_skill_trigger(const std::string &content, std::string &name)
{
    std::istringstream iss(content);
    std::string line;
    const std::string marker = "show_skill:";
    while (std::getline(iss, line))
    {
        std::string trimmed = trim(line);
        if (starts_with(trimmed, marker))
        {
            std::string candidate = trim(trimmed.substr(marker.size()));
            if (!candidate.empty())
            {
                name = candidate;
                return true;
            }
        }
    }
    return false;
}

} // namespace tinyca
