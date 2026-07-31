// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "../agent.h"
#include "../json.h"

#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

namespace {

    bool require(const bool condition, const std::string_view message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    std::string jsonQuoted(const std::string_view value) {
        std::string result;
        microcodex::json::appendString(result, value);
        return result;
    }

    const microcodex::ToolBase *findTool(
        const std::vector<std::shared_ptr<const microcodex::ToolBase>> &tools,
        const std::string_view name) {
        for (const auto &tool : tools) {
            if (tool->name() == name) {
                return tool.get();
            }
        }
        return nullptr;
    }

    bool run() {
        const auto tools = microcodex::makeCodingTools();
        bool passed = require(tools.size() == 5, "exactly five coding tools are registered");
        for (const std::string_view name : {"read", "write", "edit", "glob", "bash"}) {
            const auto *tool = findTool(tools, name);
            passed &= require(tool != nullptr, "expected tool is registered");
            if (tool != nullptr) {
                passed &= require(tool->toJsonString().find("\"name\":\"" + std::string(name) + '\"') != std::string::npos,
                                  "tool schema has its tool name");
            }
        }
        if (!passed) {
            return false;
        }

        std::filesystem::path directory;
        std::error_code error;
        std::random_device random;
        bool created_directory = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            directory = std::filesystem::temp_directory_path() /
                        ("microcodex-agent-test-" + std::to_string(random()));
            error.clear();
            if (std::filesystem::create_directory(directory, error)) {
                created_directory = true;
                break;
            }
            if (error != std::errc::file_exists) {
                break;
            }
        }
        if (!require(created_directory, "temporary test directory was created")) {
            return false;
        }
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{directory};

        const std::string path = (directory / "note.txt").string();
        const auto *write = findTool(tools, "write");
        const auto *read = findTool(tools, "read");
        const auto *edit = findTool(tools, "edit");
        const auto *glob = findTool(tools, "glob");
        const auto *bash = findTool(tools, "bash");

        auto write_result = write->executeJson("{\"path\":" + jsonQuoted(path) + ",\"content\":\"one two\"}");
        passed &= require(write_result && *write_result == "Created " + path, "write creates a new file");

        auto read_result = read->executeJson("{\"path\":" + jsonQuoted(path) + ",\"offset\":4,\"limit\":3}");
        passed &= require(read_result && *read_result == "two", "read honors offset and limit");

        auto edit_result = edit->executeJson("{\"path\":" + jsonQuoted(path) +
                                             ",\"old_content\":\"two\",\"new_content\":\"three\",\"replace_all\":false}");
        passed &= require(edit_result && *edit_result == "Edited " + path, "edit replaces exact content");

        auto glob_result = glob->executeJson("{\"pattern\":" + jsonQuoted((directory / "*.txt").string()) + '}');
        passed &= require(glob_result && *glob_result == path, "glob returns matching path");

        auto bash_result = bash->executeJson(R"({"command":"sh -c 'printf out; printf err >&2; exit 7'"})");
        passed &= require(bash_result && *bash_result == R"({"stdout":"out","stderr":"err","exit_code":7})",
                          "bash returns stdout, stderr, and non-zero exit code");
        return passed;
    }

} // namespace

int main() {
    return run() ? 0 : 1;
}
