// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

namespace tinyca
{

std::string trim(const std::string &s);
std::vector<std::string> split(const std::string &s, char delim);
std::string to_lower(const std::string &s);
bool starts_with(const std::string &s, const std::string &prefix);

} // namespace tinyca
