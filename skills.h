// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microcodex {

    struct SkillMetadata {
        std::string name;
        std::string description;
        std::filesystem::path path;
    };

    // Codex's skill installer uses $CODEX_HOME/skills, defaulting to
    // ~/.codex/skills. An empty result means no usable shared skill root exists.
    std::filesystem::path codexSkillsDirectory();

    // Recursively discovers SKILL.md files and reads only their YAML-frontmatter
    // name and description. Invalid or unreadable skills are omitted.
    std::vector<SkillMetadata> discoverCodexSkills();

    // Returns the developer-instruction catalog consumed by the base prompt,
    // or an empty string when no valid skills are installed.
    std::string availableSkillsInstructions();

} // namespace microcodex
