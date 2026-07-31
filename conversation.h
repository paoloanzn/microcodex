// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex {

    struct TurnBoundary {
        std::uint64_t number;
        std::size_t end;
    };

    struct ConversationMetadata {
        std::uint32_t version = 1;
        std::string id;
        std::string created_at;
        std::string working_directory;
        std::string model;
    };

    struct SavedTurn {
        std::uint64_t number;
        std::string id;
        std::vector<std::string> items;
    };

    struct ResumedConversation {
        ConversationMetadata metadata;
        std::vector<std::string> input_items;
        std::vector<TurnBoundary> completed_turns;
        bool has_summary = false;
        std::uint64_t compaction_generation = 0;
        std::uint64_t next_turn_number = 1;
    };

    struct ConversationSummary {
        ConversationMetadata metadata;
        std::filesystem::path path;
    };

    struct CompactionCheckpoint {
        std::uint64_t generation;
        std::uint64_t through_turn;
        std::uint64_t keep_from_turn;
        std::string summary;
    };

    std::expected<std::filesystem::path, std::string> conversationDirectory();
    std::expected<ConversationMetadata, std::string> readConversationMetadata(const std::filesystem::path &path);
    std::expected<std::vector<ConversationSummary>, std::string> listConversations(const std::filesystem::path &directory);

    // ConversationFile is the sole durable representation of a conversation.
    // It stores completed turns and compaction checkpoints; incomplete turns
    // deliberately remain only in CodexApi memory.
    class ConversationFile {
    public:
        static std::expected<ConversationFile, std::string> create(const std::filesystem::path &directory, const ConversationMetadata &metadata);

        static std::expected<ConversationFile, std::string> open(const std::filesystem::path &path);

        static std::expected<ConversationFile, std::string> openReadOnly(const std::filesystem::path &path);

        ConversationFile(const ConversationFile &) = delete;
        ConversationFile &operator=(const ConversationFile &) = delete;
        ConversationFile(ConversationFile &&other) noexcept;
        ConversationFile &operator=(ConversationFile &&other) noexcept;
        ~ConversationFile();

        std::expected<ResumedConversation, std::string> resume();

        std::expected<void, std::string> appendTurn(std::uint64_t number, std::string_view id, std::span<const std::string> items);

        std::expected<void, std::string> appendCheckpoint(const CompactionCheckpoint &checkpoint);

        std::expected<std::vector<SavedTurn>, std::string> readTurnsBefore(std::size_t cursor, std::size_t maximum_bytes) const;

        [[nodiscard]] const std::filesystem::path &path() const { return path_; }
        [[nodiscard]] std::size_t turnCount() const { return turn_locations_.size(); }

    private:
        struct TurnLocation {
            std::uint64_t number;
            std::uint64_t offset;
            std::uint64_t length;
        };

        ConversationFile(std::filesystem::path path, int descriptor, std::uint64_t file_size);

        std::expected<void, std::string> appendRecord(std::string_view record);
        std::expected<SavedTurn, std::string> readTurn(const TurnLocation &location) const;

        std::filesystem::path path_;
        int descriptor_ = -1;
        std::uint64_t file_size_ = 0;
        std::vector<TurnLocation> turn_locations_;
    };

} // namespace microcodex
