// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace tinyca
{

// Parses `key = value` INI files: '#' or ';' comments, [section] headers,
// trimmed values, no quoting, no \n escaping in values. SPEC.md §4.
//
// Keys outside of any [section] header land in the "" section (so the output
// key is just ".key") -- Config::apply treats that as unknown and warns,
// without the parser needing to know what a "valid" section is.
class IniParser
{
  public:
    // Output keys are "section.key", exactly as written (no case-folding).
    // Returns false only on an I/O error reading an existing file; a missing
    // file is reported via `file_found = false`, not as a parse failure --
    // callers decide whether that's fatal (SPEC.md's load order makes it not).
    static bool parse_file(const std::string &path, std::map<std::string, std::string> &out,
                           std::vector<std::string> &warnings, bool &file_found);
};

} // namespace tinyca
