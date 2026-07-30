// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "read.h"

#include <algorithm>
#include <cstddef>
#include <ios>
#include <expected>
#include <fstream>
#include <string>

namespace {

    size_t bytesToRead(size_t size, size_t offset, size_t limit) {
        return std::min(limit, size - offset);
    }

} // namespace

namespace microcodex {

    std::expected<std::string, std::string> read(const std::string& path, size_t offset, size_t limit) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file) {
            return std::unexpected("Could not open file");
        }

        const std::streamsize stream_size = file.tellg();
        if (stream_size< 0) {
            return std::unexpected("Could not get file size");
        }

        size_t size = static_cast<size_t>(stream_size);
        if (offset > size) {
            return std::unexpected("Offset is beyond file size");
        }

        file.seekg(offset, std::ios::beg);
        if (!file) {
            return std::unexpected("Could not seek in file");
        }

        const size_t length = bytesToRead(size, offset, limit);
        std::string data(length, '\0');

        if (length > 0 && !file.read(data.data(), static_cast<std::streamsize>(length))) {
            return std::unexpected("Could not read file");
        }

        return data;
    }

} // namespace microcodex
