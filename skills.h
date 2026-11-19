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

// $CODEX_HOME, or ~/.codex when CODEX_HOME is unset. Empty when neither
// CODEX_HOME nor HOME is set.
std::filesystem::path codexHomeDirectory();

// Codex's skill installer uses $CODEX_HOME/skills, defaulting to
// ~/.codex/skills. An empty result means no usable shared skill root exists.
std::filesystem::path codexSkillsDirectory();

// Recursively discovers SKILL.md files and reads only their YAML-frontmatter
// name and description. Invalid or unreadable skills are omitted.
std::vector<SkillMetadata> discoverCodexSkills();

// Returns the developer-instruction catalog consumed by the base prompt,
// or an empty string when no valid skills are installed.
std::string availableSkillsInstructions();

// The user's instructions for every project: the trimmed text of
// $CODEX_HOME/AGENTS.override.md, or of $CODEX_HOME/AGENTS.md when the
// override is missing, unreadable or blank. Empty when neither has any text.
// A file over 32 KiB is cut, and a <truncated> note with its size and path
// is appended so the model knows the rest exists.
std::string loadGlobalInstructions();

}  // namespace microcodex
