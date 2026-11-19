// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "skills.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace microcodex {

namespace {

constexpr std::size_t maximum_skill_file_bytes = std::size_t{1024} * 1024;
constexpr std::size_t maximum_skill_entries = 20'000;
constexpr int maximum_scan_depth = 6;
constexpr std::size_t maximum_global_instructions_bytes = std::size_t{32} * 1024;

std::string_view trim(std::string_view value) {
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.front())) != 0)) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) != 0)) {
        value.remove_suffix(1);
    }
    return value;
}

std::string yamlScalar(std::string_view value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        const char quote = value.front();
        value.remove_prefix(1);
        value.remove_suffix(1);
        std::string decoded;
        decoded.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (quote == '\'' && value[index] == '\'' && index + 1 < value.size() && value[index + 1] == '\'') {
                decoded += '\'';
                ++index;
            } else if (quote == '"' && value[index] == '\\' && index + 1 < value.size()) {
                const char escaped = value[++index];
                if (escaped == 'n')
                    decoded += '\n';
                else if (escaped == 't')
                    decoded += '\t';
                else
                    decoded += escaped;
            } else {
                decoded += value[index];
            }
        }
        return decoded;
    }
    return std::string(value);
}

struct FrontMatter {
    std::string name;
    std::string description;
    bool closed = false;
};

// The front matter field a key sets, or null for keys skills do not use.
std::string* frontMatterField(FrontMatter& front_matter, const std::string_view key) {
    if (key == "name") return &front_matter.name;
    if (key == "description") return &front_matter.description;
    return nullptr;
}

// Read the name and description from the YAML front matter that follows the
// opening "---" line, up to the closing "---". Folded and literal block
// scalars (">", ">-", "|", "|-") join their indented lines with spaces.
FrontMatter readFrontMatter(std::istream& input) {
    FrontMatter front_matter;
    std::string line;
    std::string* folded = nullptr;
    while (std::getline(input, line)) {
        if (trim(line) == "---") {
            front_matter.closed = true;
            break;
        }

        const bool indented = !line.empty() && (std::isspace(static_cast<unsigned char>(line.front())) != 0);
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
        std::string* target = frontMatterField(front_matter, key);
        if (target == nullptr) continue;

        if (value == ">" || value == ">-" || value == "|" || value == "|-") {
            target->clear();
            folded = target;
        } else {
            *target = yamlScalar(value);
        }
    }
    return front_matter;
}

bool isValidFrontMatter(const FrontMatter& front_matter) {
    const std::string& name = front_matter.name;
    const std::string& description = front_matter.description;
    return front_matter.closed && !name.empty() && !description.empty() && name.size() <= 128 &&
           description.size() <= 1024 && name.find_first_of("\r\n") == std::string::npos &&
           description.find_first_of("\r\n") == std::string::npos;
}

std::optional<SkillMetadata> readSkillMetadata(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_skill_file_bytes) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    std::string line;
    if (!std::getline(input, line) || trim(line) != "---") return std::nullopt;

    FrontMatter front_matter = readFrontMatter(input);
    if (!isValidFrontMatter(front_matter)) {
        return std::nullopt;
    }

    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
    return SkillMetadata{.name = std::move(front_matter.name),
                         .description = std::move(front_matter.description),
                         .path = absolute.lexically_normal()};
}

}  // namespace

std::filesystem::path codexHomeDirectory() {
    if (const char* codex_home = std::getenv("CODEX_HOME"); codex_home != nullptr && *codex_home != '\0') {
        return codex_home;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".codex";
    }
    return {};
}

std::filesystem::path codexSkillsDirectory() {
    const std::filesystem::path home = codexHomeDirectory();
    return home.empty() ? home : home / "skills";
}

std::vector<SkillMetadata> discoverCodexSkills() {
    const std::filesystem::path root = codexSkillsDirectory();
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error) || error) return {};

    std::vector<SkillMetadata> skills;
    std::size_t entries = 0;
    const auto options = std::filesystem::directory_options::follow_directory_symlink |
                         std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(root, options, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end && entries < maximum_skill_entries) {
        ++entries;
        const auto& entry = *iterator;
        if (iterator.depth() >= maximum_scan_depth && entry.is_directory(error)) {
            iterator.disable_recursion_pending();
        }
        if (!error && entry.is_regular_file(error) && entry.path().filename() == "SKILL.md") {
            if (auto skill = readSkillMetadata(entry.path())) skills.push_back(std::move(*skill));
        }
        error.clear();
        iterator.increment(error);
    }

    std::ranges::sort(skills, [](const SkillMetadata& left, const SkillMetadata& right) {
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
    for (const SkillMetadata& skill : skills) {
        catalog += "- " + skill.name + ": " + skill.description + " (file: " + skill.path.string() + ")\n";
    }
    return catalog;
}

std::string loadGlobalInstructions() {
    const std::filesystem::path home = codexHomeDirectory();
    if (home.empty()) return {};

    for (const char* name : {"AGENTS.override.md", "AGENTS.md"}) {
        const std::filesystem::path path = home / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;

        std::ifstream input(path, std::ios::binary);
        std::string text(maximum_global_instructions_bytes + 1, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.bad() || (!input && !input.eof())) continue;
        text.resize(static_cast<std::size_t>(input.gcount()));

        const bool cut = text.size() > maximum_global_instructions_bytes;
        if (cut) {
            // Cut before a UTF-8 continuation byte so no character is split.
            std::size_t end = maximum_global_instructions_bytes;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
            text.resize(end);
        }
        std::string loaded(trim(text));
        if (loaded.empty()) continue;
        if (cut) {
            // Tell the model what is missing and where to read the rest.
            const auto size = std::filesystem::file_size(path, error);
            const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
            loaded += "\n<truncated>Only the first " + std::to_string(text.size()) + " of " +
                      (error ? "more" : std::to_string(size)) + " bytes of " +
                      (absolute.empty() ? path : absolute).string() + " were loaded.</truncated>";
        }
        return loaded;
    }
    return {};
}

}  // namespace microcodex
