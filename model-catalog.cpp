// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "model-catalog.h"

#include "http.h"
#include "json.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace microcodex {

    namespace {

        constexpr std::size_t default_effective_context_window_percent = 95;
        constexpr std::size_t default_auto_compact_percent = 90;
        constexpr std::size_t maximum_models_response_bytes = 2 * 1024 * 1024;
        // The backend filters models by their minimum Codex client version.
        // Keep this aligned with the Codex protocol version we implement.
        constexpr std::string_view models_client_version = "0.146.0";

        std::expected<std::optional<std::size_t>, std::string> optionalSizeMember(const std::string_view object, const std::string_view name) {
            auto member = json::findJsonMember(object, name);
            if (!member) return std::unexpected(member.error());
            if (!*member || **member == "null") return std::optional<std::size_t>{};

            std::size_t value = 0;
            const char *begin = (**member).data();
            const char *end = begin + (**member).size();
            const auto [parsed_end, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed_end != end) {
                return std::unexpected("JSON member '" + std::string(name) + "' is not an unsigned integer");
            }
            return std::optional<std::size_t>{value};
        }

        std::size_t percentage(const std::size_t value, const std::size_t percent) {
            return value / 100 * percent + value % 100 * percent / 100;
        }

        std::expected<std::string, std::string> modelsEndpoint(const std::string_view responses_endpoint) {
            const std::size_t suffix = responses_endpoint.find_first_of("?#");
            std::string endpoint(responses_endpoint.substr(0, suffix));
            constexpr std::string_view responses_path = "/responses";
            if (!endpoint.ends_with(responses_path)) {
                return std::unexpected("Responses endpoint does not end with /responses");
            }
            endpoint.resize(endpoint.size() - responses_path.size());
            endpoint += "/models?client_version=";
            endpoint += models_client_version;
            return endpoint;
        }

        std::string modelsError(const std::string_view body, const long status) {
            auto error = json::findJsonMember(body, "error");
            if (error && *error) {
                auto message = json::jsonStringMember(**error, "message");
                if (message && *message) return "Models API returned HTTP " + std::to_string(status) + ": " + **message;
            }
            return "Models API returned HTTP " + std::to_string(status);
        }

    } // namespace

    std::expected<std::vector<ModelContextLimits>, std::string> parseModelContextLimits(const std::string_view response) {
        auto models_member = json::findJsonMember(response, "models");
        if (!models_member) return std::unexpected(models_member.error());
        if (!*models_member) return std::unexpected("Models response has no models array");
        auto elements = json::jsonArrayElements(**models_member);
        if (!elements) return std::unexpected(elements.error());

        std::vector<ModelContextLimits> models;
        models.reserve(elements->size());
        for (const std::string_view element : *elements) {
            auto name = json::requiredJsonString(element, "slug");
            auto context_window = optionalSizeMember(element, "context_window");
            auto maximum_context_window = optionalSizeMember(element, "max_context_window");
            auto compact_at = optionalSizeMember(element, "auto_compact_token_limit");
            auto effective_percent = optionalSizeMember(element, "effective_context_window_percent");
            if (!name || !context_window || !maximum_context_window || !compact_at || !effective_percent) {
                if (!name) return std::unexpected(name.error());
                if (!context_window) return std::unexpected(context_window.error());
                if (!maximum_context_window) return std::unexpected(maximum_context_window.error());
                if (!compact_at) return std::unexpected(compact_at.error());
                return std::unexpected(effective_percent.error());
            }

            const std::optional<std::size_t> resolved_window = *context_window ? *context_window : *maximum_context_window;
            if (!resolved_window || *resolved_window == 0) {
                return std::unexpected("Model '" + *name + "' has no context window");
            }
            const std::size_t maximum_window = maximum_context_window->value_or(*resolved_window);
            const std::size_t usable_percent = effective_percent->value_or(default_effective_context_window_percent);
            if (maximum_window < *resolved_window || usable_percent == 0 || usable_percent > 100) {
                return std::unexpected("Model '" + *name + "' has invalid context limits");
            }
            const std::size_t default_compact_at = percentage(*resolved_window, default_auto_compact_percent);
            const std::size_t resolved_compact_at = std::min(compact_at->value_or(default_compact_at), default_compact_at);
            if (resolved_compact_at == 0) {
                return std::unexpected("Model '" + *name + "' has an invalid compaction limit");
            }

            models.push_back({
                .name = std::move(*name),
                .context_window_tokens = *resolved_window,
                .maximum_context_window_tokens = maximum_window,
                .effective_context_window_percent = usable_percent,
                .effective_context_window_tokens = percentage(*resolved_window, usable_percent),
                .compact_at_tokens = resolved_compact_at,
            });
        }
        return models;
    }

    std::expected<std::vector<ModelContextLimits>, std::string> fetchModelContextLimits(const std::string_view responses_endpoint, const std::string_view access_token, const std::string_view account_id) {
        auto endpoint = modelsEndpoint(responses_endpoint);
        if (!endpoint) return std::unexpected(endpoint.error());

        std::vector<std::string> headers{
            "Authorization: Bearer " + std::string(access_token),
            "Accept: application/json",
            "originator: codex_cli_rs",
        };
        if (!account_id.empty()) headers.push_back("ChatGPT-Account-ID: " + std::string(account_id));
        auto response = performHttpRequest({
            .method = HttpMethod::Get,
            .url = *endpoint,
            .headers = headers,
            .body = {},
            .idle_timeout_seconds = 5,
            .total_timeout_seconds = 5,
            .maximum_response_bytes = maximum_models_response_bytes,
            .stop_token = {},
        });
        if (!response) return std::unexpected("Could not retrieve model context limits: " + response.error());
        if (response->status < 200 || response->status >= 300) return std::unexpected(modelsError(response->body, response->status));
        return parseModelContextLimits(response->body);
    }

    const ModelContextLimits *findModelContextLimits(const std::span<const ModelContextLimits> models, const std::string_view name) noexcept {
        const auto model = std::find_if(models.begin(), models.end(), [name](const ModelContextLimits &candidate) { return candidate.name == name; });
        return model == models.end() ? nullptr : &*model;
    }

    CompactionConfig compactionConfigForModel(const ModelContextLimits &model, const std::size_t retained_context_tokens, const std::size_t maximum_summary_bytes) {
        return {
            .context_limit_tokens = model.effective_context_window_tokens,
            .compact_at_tokens = model.compact_at_tokens,
            .retained_context_tokens = retained_context_tokens,
            .maximum_summary_bytes = maximum_summary_bytes,
        };
    }

} // namespace microcodex
