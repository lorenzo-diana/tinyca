// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/resume.h"

#include <fstream>
#include <sstream>

#include "json.hpp"

namespace tinyca
{

namespace
{

using nlohmann::json;

// The system prompt is not logged as an entry of its own; it only appears as
// messages[0] of each logged request (role "system"). The FIRST request is the
// one that reflects the prompt the session actually started with -- later ones
// repeat it, but reading the first keeps the answer stable even if a future
// version allows the prompt to change mid-session.
bool system_prompt_from_request(const json &payload, std::string &out)
{
    if (!payload.contains("messages") || !payload["messages"].is_array() || payload["messages"].empty())
        return false;
    const json &first = payload["messages"].front();
    if (!first.is_object() || first.value("role", std::string()) != "system")
        return false;
    out = first.value("content", std::string());
    return true;
}

} // namespace

bool load_session(const std::string &path, ResumedSession &out, std::string &err)
{
    std::ifstream in(path);
    if (!in)
    {
        err = "cannot open transcript file: " + path;
        return false;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        json j;
        try
        {
            j = json::parse(line);
        }
        catch (...)
        {
            continue; // skip malformed/truncated lines (e.g. a cutoff marker)
        }
        const std::string kind = j.value("kind", "");
        if (!j.contains("payload"))
            continue;
        const json &payload = j["payload"];

        if (kind == "session_start" || kind == "work_dir_update")
        {
            // The LAST one wins: a session that moved directories records a
            // work_dir_update, and what matters is where it ran most recently.
            if (payload.is_object() && payload.contains("work_dir"))
            {
                out.stored_work_dir = payload.value("work_dir", std::string());
                out.found_work_dir = !out.stored_work_dir.empty();
            }
            continue;
        }

        if (kind == "request")
        {
            // Not replayed, but it is where the stored system prompt lives.
            if (!out.found_system_prompt && payload.is_object())
                out.found_system_prompt = system_prompt_from_request(payload, out.stored_system_prompt);
            continue;
        }

        Message m;
        if (kind == "user_task")
        {
            m.role = "user";
            m.content = payload.value("text", "");
        }
        else if (kind == "response")
        {
            m.role = "assistant";
            m.content = payload.value("content", "");
        }
        else if (kind == "tool_result")
        {
            m.role = "tool";
            m.content = payload.value("content", "");
        }
        else
        {
            continue; // skills_changed, tier2_skill, transcript_cutoff, ...
        }
        out.history.push_back(m);
    }
    return true;
}

std::string describe_prompt_difference(const std::string &stored, const std::string &current)
{
    if (stored == current)
        return "identical";

    std::istringstream a(stored);
    std::istringstream b(current);
    std::string la;
    std::string lb;
    int line_no = 0;
    while (true)
    {
        const bool got_a = static_cast<bool>(std::getline(a, la));
        const bool got_b = static_cast<bool>(std::getline(b, lb));
        if (!got_a && !got_b)
            break;
        ++line_no;
        if (!got_a || !got_b || la != lb)
        {
            std::ostringstream ss;
            ss << "first difference at line " << line_no << " (stored: " << stored.size()
               << " bytes, current: " << current.size() << " bytes)";
            return ss.str();
        }
    }
    // Same lines but different text: only trailing-newline differences remain.
    std::ostringstream ss;
    ss << "same lines, different trailing whitespace (stored: " << stored.size()
       << " bytes, current: " << current.size() << " bytes)";
    return ss.str();
}

} // namespace tinyca
