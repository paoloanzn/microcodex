// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "skills.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace microcodex {

    namespace {

        constexpr std::size_t maximum_skill_file_bytes = 1024 * 1024;
        constexpr std::size_t maximum_skill_entries = 20'000;
        constexpr int maximum_scan_depth = 6;

        std::string_view trim(std::string_view value) {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                value.remove_suffix(1);
            }
            return value;
        }

        std::string yamlScalar(std::string_view value) {
            value = trim(value);
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\''))) {
                const char quote = value.front();
                value.remove_prefix(1);
                value.remove_suffix(1);
                std::string decoded;
                decoded.reserve(value.size());
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (quote == '\'' && value[index] == '\'' && index + 1 < value.size() &&
                        value[index + 1] == '\'') {
                        decoded += '\'';
                        ++index;
                    } else if (quote == '"' && value[index] == '\\' && index + 1 < value.size()) {
                        const char escaped = value[++index];
                        if (escaped == 'n') decoded += '\n';
                        else if (escaped == 't') decoded += '\t';
                        else decoded += escaped;
                    } else {
                        decoded += value[index];
                    }
                }
                return decoded;
            }
            return std::string(value);
        }

        std::optional<SkillMetadata> readSkillMetadata(const std::filesystem::path &path) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > maximum_skill_file_bytes) return std::nullopt;

            std::ifstream input(path, std::ios::binary);
            if (!input) return std::nullopt;

            std::string line;
            if (!std::getline(input, line) || trim(line) != "---") return std::nullopt;

            std::string name;
            std::string description;
            std::string *folded = nullptr;
            bool closed = false;
            while (std::getline(input, line)) {
                if (trim(line) == "---") {
                    closed = true;
                    break;
                }

                const bool indented = !line.empty() && std::isspace(static_cast<unsigned char>(line.front()));
                if (folded != nullptr && indented) {
                    const std::string_view continuation = trim(line);
                    if (!continuation.empty()) {
                        if (!folded->empty()) *folded += ' ';
                        folded->append(continuation);
                    }
                    continue;
                }
                folded = nullptr;

                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                const std::string_view key = trim(std::string_view(line).substr(0, colon));
                const std::string_view value = trim(std::string_view(line).substr(colon + 1));
                std::string *target = nullptr;
                if (key == "name") target = &name;
                else if (key == "description") target = &description;
                if (target == nullptr) continue;

                if (value == ">" || value == ">-" || value == "|" || value == "|-") {
                    target->clear();
                    folded = target;
                } else {
                    *target = yamlScalar(value);
                }
            }

            if (!closed || name.empty() || description.empty() || name.size() > 128 ||
                description.size() > 1024 || name.find_first_of("\r\n") != std::string::npos ||
                description.find_first_of("\r\n") != std::string::npos) {
                return std::nullopt;
            }

            const std::filesystem::path absolute = std::filesystem::absolute(path, error);
            if (error) return std::nullopt;
            return SkillMetadata{std::move(name), std::move(description), absolute.lexically_normal()};
        }

    } // namespace

    std::filesystem::path codexSkillsDirectory() {
        if (const char *codex_home = std::getenv("CODEX_HOME");
            codex_home != nullptr && codex_home[0] != '\0') {
            return std::filesystem::path(codex_home) / "skills";
        }
        if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::filesystem::path(home) / ".codex" / "skills";
        }
        return {};
    }

    std::vector<SkillMetadata> discoverCodexSkills() {
        const std::filesystem::path root = codexSkillsDirectory();
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error) || error) return {};

        std::vector<SkillMetadata> skills;
        std::size_t entries = 0;
        const auto options = std::filesystem::directory_options::follow_directory_symlink |
                             std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
        while (!error && iterator != end && entries < maximum_skill_entries) {
            ++entries;
            const auto &entry = *iterator;
            if (iterator.depth() >= maximum_scan_depth && entry.is_directory(error)) {
                iterator.disable_recursion_pending();
            }
            if (!error && entry.is_regular_file(error) && entry.path().filename() == "SKILL.md") {
                if (auto skill = readSkillMetadata(entry.path())) skills.push_back(std::move(*skill));
            }
            error.clear();
            iterator.increment(error);
        }

        std::sort(skills.begin(), skills.end(), [](const SkillMetadata &left, const SkillMetadata &right) {
            if (left.name != right.name) return left.name < right.name;
            return left.path < right.path;
        });
        return skills;
    }

    std::string availableSkillsInstructions() {
        const std::vector<SkillMetadata> skills = discoverCodexSkills();
        if (skills.empty()) return {};

        std::string catalog =
            "\n\n## Skills\n"
            "A skill is a set of local instructions stored in a `SKILL.md` file. "
            "Below are the skills available in this session. Each entry includes a name, "
            "description, and absolute file path.\n\n"
            "### Available skills\n";
        for (const SkillMetadata &skill : skills) {
            catalog += "- " + skill.name + ": " + skill.description + " (file: " +
                       skill.path.string() + ")\n";
        }
        return catalog;
    }

} // namespace microcodex
