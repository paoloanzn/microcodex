// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "write.h"

#include <cstddef>
#include <expected>
#include <fstream> 
#include <iostream>

namespace {

    bool doesFileExist(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        return true;
    }

} // namespace

namespace microcodex {

    std::expected<int, std::string> write(const std::string& path, std::string_view content) {
        if (doesFileExist(path)) {
            return std::unexpected("Overwriting an existing file is not allowed");
        }

        std::ofstream file(path, std::ios::out  | std::ios::trunc);
        if (!file) {
            return std::unexpected("Could not create file");
        }

        file << content;

        if (!file) {
            return std::unexpected("Failed to write to file");
        }

        return NULL;
    }

} // namespace microcodex