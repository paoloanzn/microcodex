// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "conversation.h"

#include "json.h"
#include "response-item.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace microcodex {

    namespace {

        constexpr std::size_t maximum_record_bytes = std::size_t{16} * 1024 * 1024;

        std::string systemError(const std::string_view operation) {
            return std::string(operation) + ": " + std::strerror(errno);
        }

        std::expected<std::uint64_t, std::string> unsignedMember(const std::string_view object, const std::string_view name) {
            auto member = json::scalarMember(object, name);
            if (!member) {
                return std::unexpected(member.error());
            }
            std::uint64_t value = 0;
            const char *begin = member->data();
            const char *end = begin + member->size();
            const auto [parsed_end, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed_end != end) {
                return std::unexpected("JSON member '" + std::string(name) +
                                       "' is not an unsigned integer");
            }
            return value;
        }

        std::expected<std::vector<std::string>, std::string> itemArray(const std::string_view object) {
            auto member = json::scalarMember(object, "items");
            if (!member) {
                return std::unexpected(member.error());
            }
            auto elements = json::jsonArrayElements(*member);
            if (!elements) {
                return std::unexpected(elements.error());
            }
            std::vector<std::string> items;
            items.reserve(elements->size());
            for (const std::string_view element : *elements) {
                items.emplace_back(element);
            }
            return items;
        }

        std::expected<ConversationMetadata, std::string> parseMetadata(const std::string_view line) {
            auto type = json::requiredJsonString(line, "type");
            auto version = unsignedMember(line, "version");
            auto id = json::requiredJsonString(line, "id");
            auto created_at = json::requiredJsonString(line, "created_at");
            auto working_directory = json::requiredJsonString(line, "working_directory");
            auto model = json::requiredJsonString(line, "model");
            if (!type || !version || !id || !created_at || !working_directory || !model) {
                if (!type) return std::unexpected(type.error());
                if (!version) return std::unexpected(version.error());
                if (!id) return std::unexpected(id.error());
                if (!created_at) return std::unexpected(created_at.error());
                if (!working_directory) return std::unexpected(working_directory.error());
                return std::unexpected(model.error());
            }
            if (*type != "conversation") {
                return std::unexpected("Conversation file does not begin with metadata");
            }
            if (*version != 1 || *version > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected("Unsupported conversation file version " +
                                       std::to_string(*version));
            }
            return ConversationMetadata{
                .version = static_cast<std::uint32_t>(*version),
                .id = std::move(*id),
                .created_at = std::move(*created_at),
                .working_directory = std::move(*working_directory),
                .model = std::move(*model),
            };
        }

        std::expected<SavedTurn, std::string> parseTurn(const std::string_view line) {
            auto number = unsignedMember(line, "number");
            auto id = json::requiredJsonString(line, "id");
            auto items = itemArray(line);
            if (!number || !id || !items) {
                if (!number) return std::unexpected(number.error());
                if (!id) return std::unexpected(id.error());
                return std::unexpected(items.error());
            }
            if (items->empty()) {
                return std::unexpected("Saved turn contains no response items");
            }
            return SavedTurn{
                .number = *number,
                .id = std::move(*id),
                .items = std::move(*items),
            };
        }

        std::expected<CompactionCheckpoint, std::string> parseCheckpoint(const std::string_view line) {
            auto generation = unsignedMember(line, "generation");
            auto through_turn = unsignedMember(line, "through_turn");
            auto keep_from_turn = unsignedMember(line, "keep_from_turn");
            auto summary = json::requiredJsonString(line, "summary");
            if (!generation || !through_turn || !keep_from_turn || !summary) {
                if (!generation) return std::unexpected(generation.error());
                if (!through_turn) return std::unexpected(through_turn.error());
                if (!keep_from_turn) return std::unexpected(keep_from_turn.error());
                return std::unexpected(summary.error());
            }
            if (*keep_from_turn == 0 || *keep_from_turn > *through_turn + 1) {
                return std::unexpected("Compaction checkpoint has an invalid retained turn");
            }
            return CompactionCheckpoint{
                .generation = *generation,
                .through_turn = *through_turn,
                .keep_from_turn = *keep_from_turn,
                .summary = std::move(*summary),
            };
        }

        std::string metadataRecord(const ConversationMetadata &metadata) {
            std::string record = "{\"type\":\"conversation\",\"version\":" +
                                 std::to_string(metadata.version) + ",\"id\":";
            json::appendJsonString(record, metadata.id);
            record += ",\"created_at\":";
            json::appendJsonString(record, metadata.created_at);
            record += ",\"working_directory\":";
            json::appendJsonString(record, metadata.working_directory);
            record += ",\"model\":";
            json::appendJsonString(record, metadata.model);
            record += '}';
            return record;
        }

        std::string turnRecord(const std::uint64_t number, const std::string_view id, const std::span<const std::string> items) {
            std::string record = "{\"type\":\"turn\",\"number\":" +
                                 std::to_string(number) + ",\"id\":";
            json::appendJsonString(record, id);
            record += ",\"items\":[";
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (index != 0) record += ',';
                record += items[index];
            }
            record += "]}";
            return record;
        }

        std::string checkpointRecord(const CompactionCheckpoint &checkpoint) {
            std::string record = "{\"type\":\"checkpoint\",\"generation\":" +
                                 std::to_string(checkpoint.generation) +
                                 ",\"through_turn\":" +
                                 std::to_string(checkpoint.through_turn) +
                                 ",\"keep_from_turn\":" +
                                 std::to_string(checkpoint.keep_from_turn) +
                                 ",\"summary\":";
            json::appendJsonString(record, checkpoint.summary);
            record += '}';
            return record;
        }

        std::expected<void, std::string> writeAll(const int descriptor, const std::string_view bytes) {
            std::size_t written = 0;
            while (written < bytes.size()) {
                const ssize_t count = ::write(descriptor, bytes.data() + written,
                                              bytes.size() - written);
                if (count < 0) {
                    if (errno == EINTR) continue;
                    return std::unexpected(systemError("Could not write conversation"));
                }
                written += static_cast<std::size_t>(count);
            }
            return {};
        }

        std::expected<std::string, std::string> readRecord(const std::filesystem::path &path, const std::uint64_t offset, const std::uint64_t length) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::unexpected("Could not open conversation '" + path.string() + "'");
            }
            file.seekg(static_cast<std::streamoff>(offset));
            if (!file) {
                return std::unexpected("Could not seek in conversation '" + path.string() + "'");
            }
            std::string line(static_cast<std::size_t>(length), '\0');
            file.read(line.data(), static_cast<std::streamsize>(length));
            if (file.gcount() != static_cast<std::streamsize>(length)) {
                return std::unexpected("Conversation ended while reading a saved turn");
            }
            if (!line.empty() && line.back() == '\n') line.pop_back();
            return line;
        }

    } // namespace

    std::expected<std::filesystem::path, std::string> conversationDirectory() {
        if (const char *codex_home = std::getenv("CODEX_HOME");
            codex_home != nullptr && codex_home[0] != '\0') {
            return std::filesystem::path(codex_home) / "conversations";
        }
        if (const char *user_home = std::getenv("HOME");
            user_home != nullptr && user_home[0] != '\0') {
            return std::filesystem::path(user_home) / ".codex" / "conversations";
        }
        return std::unexpected("Neither CODEX_HOME nor HOME is set");
    }

    std::expected<ConversationMetadata, std::string> readConversationMetadata(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        std::string first_line;
        if (!input || !std::getline(input, first_line)) {
            return std::unexpected("Could not read conversation metadata from '" +
                                   path.string() + "'");
        }
        return parseMetadata(first_line);
    }

    ConversationFile::ConversationFile(std::filesystem::path path, const int descriptor, const std::uint64_t file_size)
        : path_(std::move(path)), descriptor_(descriptor), file_size_(file_size) {}

    ConversationFile::ConversationFile(ConversationFile &&other) noexcept
        : path_(std::move(other.path_)),
          descriptor_(std::exchange(other.descriptor_, -1)),
          file_size_(other.file_size_),
          turn_locations_(std::move(other.turn_locations_)) {}

    ConversationFile &ConversationFile::operator=(ConversationFile &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) ::close(descriptor_);
            path_ = std::move(other.path_);
            descriptor_ = std::exchange(other.descriptor_, -1);
            file_size_ = other.file_size_;
            turn_locations_ = std::move(other.turn_locations_);
        }
        return *this;
    }

    ConversationFile::~ConversationFile() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    std::expected<ConversationFile, std::string> ConversationFile::create(const std::filesystem::path &directory, const ConversationMetadata &metadata) {
        if (metadata.id.empty() || metadata.created_at.empty() ||
            metadata.working_directory.empty() || metadata.model.empty()) {
            return std::unexpected("Conversation metadata is incomplete");
        }

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return std::unexpected("Could not create conversation directory: " +
                                   error.message());
        }
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            return std::unexpected("Could not protect conversation directory: " +
                                   error.message());
        }

        const std::filesystem::path path = directory / (metadata.id + ".jsonl");
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0600);
        if (descriptor < 0) {
            return std::unexpected(systemError("Could not create conversation"));
        }
        ConversationFile file(path, descriptor, 0);
        auto appended = file.appendRecord(metadataRecord(metadata));
        if (!appended) {
            return std::unexpected(appended.error());
        }
        return file;
    }

    std::expected<ConversationFile, std::string> ConversationFile::open(const std::filesystem::path &path) {
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(path, error);
        if (error) {
            return std::unexpected("Could not inspect conversation '" + path.string() +
                                   "': " + error.message());
        }
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_APPEND);
        if (descriptor < 0) {
            return std::unexpected(systemError("Could not open conversation for append"));
        }
        return ConversationFile(path, descriptor, size);
    }

    std::expected<ConversationFile, std::string> ConversationFile::openReadOnly(const std::filesystem::path &path) {
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(path, error);
        if (error) {
            return std::unexpected("Could not inspect conversation '" + path.string() +
                                   "': " + error.message());
        }
        return ConversationFile(path, -1, size);
    }

    std::expected<void, std::string> ConversationFile::appendRecord(const std::string_view record) {
        if (descriptor_ < 0) {
            return std::unexpected("Conversation is not open for writing");
        }
        if (record.empty() || record.size() > maximum_record_bytes) {
            return std::unexpected("Conversation record has an invalid size");
        }
        auto written = writeAll(descriptor_, record);
        if (written) written = writeAll(descriptor_, "\n");
        if (!written) {
            // A failed record is never allowed to become part of a later resume.
            // Best-effort truncation restores the last known durable boundary.
            ::ftruncate(descriptor_, static_cast<off_t>(file_size_));
            ::close(std::exchange(descriptor_, -1));
            return written;
        }
        if (::fsync(descriptor_) != 0) {
            const std::string error = systemError("Could not sync conversation");
            ::ftruncate(descriptor_, static_cast<off_t>(file_size_));
            ::close(std::exchange(descriptor_, -1));
            return std::unexpected(error);
        }
        file_size_ += record.size() + 1;
        return {};
    }

    std::expected<void, std::string> ConversationFile::appendTurn(const std::uint64_t number, const std::string_view id, const std::span<const std::string> items) {
        if (number == 0 || id.empty() || items.empty()) {
            return std::unexpected("Cannot save an incomplete turn");
        }
        const std::uint64_t expected_number =
            turn_locations_.empty() ? 1 : turn_locations_.back().number + 1;
        if (number != expected_number) {
            return std::unexpected("Conversation turns must be saved in order");
        }
        const std::string record = turnRecord(number, id, items);
        const std::uint64_t offset = file_size_;
        auto appended = appendRecord(record);
        if (!appended) return appended;
        turn_locations_.push_back({
            .number = number,
            .offset = offset,
            .length = static_cast<std::uint64_t>(record.size() + 1),
        });
        return {};
    }

    std::expected<void, std::string> ConversationFile::appendCheckpoint(const CompactionCheckpoint &checkpoint) {
        if (checkpoint.generation == 0 || checkpoint.summary.empty() ||
            checkpoint.keep_from_turn == 0 ||
            checkpoint.keep_from_turn > checkpoint.through_turn + 1) {
            return std::unexpected("Cannot save an invalid compaction checkpoint");
        }
        return appendRecord(checkpointRecord(checkpoint));
    }

    std::expected<ResumedConversation, std::string> ConversationFile::resume() {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            return std::unexpected("Could not read conversation '" + path_.string() + "'");
        }

        turn_locations_.clear();
        std::optional<ConversationMetadata> metadata;
        std::optional<CompactionCheckpoint> checkpoint;
        std::uint64_t offset = 0;
        std::uint64_t last_turn_number = 0;
        std::string line;

        while (std::getline(input, line)) {
            const std::uint64_t record_offset = offset;
            const bool terminated = record_offset + line.size() < file_size_;
            if (!terminated) {
                // An unterminated final line is a write interrupted by a crash.
                // A writable resume removes it; read-only history simply ignores it.
                if (descriptor_ >= 0) {
                    if (::ftruncate(descriptor_, static_cast<off_t>(record_offset)) != 0) {
                        return std::unexpected(systemError("Could not repair conversation tail"));
                    }
                    if (::fsync(descriptor_) != 0) {
                        return std::unexpected(systemError("Could not sync repaired conversation"));
                    }
                    file_size_ = record_offset;
                }
                break;
            }
            const std::uint64_t record_length = static_cast<std::uint64_t>(line.size() + 1);
            offset += record_length;

            if (line.size() > maximum_record_bytes) {
                return std::unexpected("Conversation record exceeds the size limit");
            }
            auto type = json::requiredJsonString(line, "type");
            if (!type) {
                return std::unexpected("Invalid conversation record at byte " +
                                       std::to_string(record_offset) + ": " + type.error());
            }

            if (!metadata) {
                auto parsed = parseMetadata(line);
                if (!parsed) return std::unexpected(parsed.error());
                metadata = std::move(*parsed);
                continue;
            }
            if (*type == "conversation") {
                return std::unexpected("Conversation contains more than one metadata record");
            }
            if (*type == "turn") {
                auto turn = parseTurn(line);
                if (!turn) return std::unexpected(turn.error());
                if (turn->number != last_turn_number + 1) {
                    return std::unexpected("Conversation turn numbers are not consecutive");
                }
                last_turn_number = turn->number;
                turn_locations_.push_back({
                    .number = turn->number,
                    .offset = record_offset,
                    .length = record_length,
                });
                continue;
            }
            if (*type == "checkpoint") {
                auto parsed = parseCheckpoint(line);
                if (!parsed) return std::unexpected(parsed.error());
                const std::uint64_t expected_generation =
                    checkpoint ? checkpoint->generation + 1 : 1;
                if (parsed->through_turn != last_turn_number ||
                    parsed->generation != expected_generation) {
                    return std::unexpected("Compaction checkpoints are inconsistent");
                }
                checkpoint = std::move(*parsed);
                continue;
            }
            return std::unexpected("Unknown conversation record type '" + *type + "'");
        }

        if (!metadata) {
            return std::unexpected("Conversation file is empty");
        }

        ResumedConversation resumed;
        resumed.metadata = std::move(*metadata);
        resumed.compaction_generation = checkpoint ? checkpoint->generation : 0;
        resumed.next_turn_number = last_turn_number + 1;

        const std::uint64_t keep_from_turn = checkpoint ? checkpoint->keep_from_turn : 1;
        if (checkpoint) {
            resumed.input_items.push_back(userMessageItem(
                "<conversation_summary>\n" + checkpoint->summary +
                "\n</conversation_summary>"));
            resumed.has_summary = true;
        }

        for (const TurnLocation &location : turn_locations_) {
            if (location.number < keep_from_turn) continue;
            auto turn = readTurn(location);
            if (!turn) return std::unexpected(turn.error());
            for (std::string &item : turn->items) {
                resumed.input_items.push_back(std::move(item));
            }
            resumed.completed_turns.push_back({
                .number = turn->number,
                .end = resumed.input_items.size(),
            });
        }
        return resumed;
    }

    std::expected<SavedTurn, std::string> ConversationFile::readTurn(const TurnLocation &location) const {
        auto record = readRecord(path_, location.offset, location.length);
        if (!record) return std::unexpected(record.error());
        auto type = json::requiredJsonString(*record, "type");
        if (!type || *type != "turn") {
            return std::unexpected("Conversation turn index points to an invalid record");
        }
        return parseTurn(*record);
    }

    std::expected<std::vector<SavedTurn>, std::string> ConversationFile::readTurnsBefore(const std::size_t cursor, const std::size_t maximum_bytes) const {
        if (maximum_bytes == 0 || turn_locations_.empty()) {
            return std::vector<SavedTurn>{};
        }
        const std::size_t end = std::min(cursor, turn_locations_.size());
        std::size_t begin = end;
        std::uint64_t bytes = 0;
        while (begin > 0) {
            const std::uint64_t next = turn_locations_[begin - 1].length;
            if (bytes != 0 &&
                (bytes >= maximum_bytes || next > maximum_bytes - bytes)) {
                break;
            }
            bytes += next;
            --begin;
        }

        std::vector<SavedTurn> turns;
        turns.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
            auto turn = readTurn(turn_locations_[index]);
            if (!turn) return std::unexpected(turn.error());
            turns.push_back(std::move(*turn));
        }
        return turns;
    }

    std::expected<std::vector<ConversationSummary>, std::string> listConversations(const std::filesystem::path &directory) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            if (error) {
                return std::unexpected("Could not inspect conversation directory: " +
                                       error.message());
            }
            return std::vector<ConversationSummary>{};
        }

        std::vector<ConversationSummary> conversations;
        for (std::filesystem::directory_iterator entries(directory, error), end;
             !error && entries != end; entries.increment(error)) {
            if (!entries->is_regular_file() || entries->path().extension() != ".jsonl") {
                continue;
            }
            auto metadata = readConversationMetadata(entries->path());
            if (!metadata) continue;
            conversations.push_back({
                .metadata = std::move(*metadata),
                .path = entries->path(),
            });
        }
        if (error) {
            return std::unexpected("Could not list conversations: " + error.message());
        }
        std::sort(conversations.begin(), conversations.end(),
                  [](const ConversationSummary &left, const ConversationSummary &right) {
                      return left.metadata.created_at > right.metadata.created_at;
                  });
        return conversations;
    }

} // namespace microcodex
