// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "read.h"
#include "write.h"
#include "bash.h"

#include <expected>
#include <iostream>


int main() {
    std::expected<std::string, std::string> file_content =
        microcodex::read("main.cpp", 0, 10000000);
    if (!file_content) {
        std::cerr << file_content.error() << "\n";
        return 1;
    }

    std::expected<int, std::string> write_result =
        microcodex::write("main.cpp.txt", file_content.value());
    if (!write_result) {
        std::cerr << write_result.error() << "\n";
        return 1;
    }
    std::cout << "done." << "\n";

    std::expected<microcodex::BashCommandResult, std::string> bash_result =
        microcodex::bash("file main.cpp.txt");
    if (!bash_result) {
        std::cerr << bash_result.error() << "\n";
        return 1;
    }
    std::cout << bash_result.value().stdout << "\n";
    return 0;
}
