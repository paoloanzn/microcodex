// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace microcodex {

    struct CodexApiConfig;

    // The complete token set returned by OpenAI. The access token and account
    // ID are the two values needed by CodexApi; the other tokens are retained
    // so a later session can refresh the access token without asking the user
    // to log in.
    struct OAuthCredentials {
        std::string access_token;
        std::string account_id;
        std::string id_token;
        std::string refresh_token;
    };

    struct OAuthOptions {
        std::string issuer = "https://auth.openai.com";
        std::string client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
        std::string originator = "codex_cli_rs";

        // These are the localhost redirect ports registered for the Codex OAuth
        // client. Port 1457 is tried only when 1455 is already occupied.
        std::uint16_t callback_port = 1455;
        std::uint16_t fallback_callback_port = 1457;
        long token_request_timeout_seconds = 30;
    };

    // Owns the short-lived localhost callback listener and the PKCE secrets for
    // one login attempt. The object is move-only so those values cannot be
    // copied accidentally.
    class OAuthLogin {
    public:
        // Binds the callback listener before constructing the URL, so the
        // browser can be opened as soon as this routine returns.
        static std::expected<OAuthLogin, std::string> start(OAuthOptions options = {});

        OAuthLogin(OAuthLogin &&) noexcept;
        OAuthLogin &operator=(OAuthLogin &&) noexcept;
        OAuthLogin(const OAuthLogin &) = delete;
        OAuthLogin &operator=(const OAuthLogin &) = delete;
        ~OAuthLogin();

        [[nodiscard]] const std::string &authorizationUrl() const;
        [[nodiscard]] std::uint16_t callbackPort() const;

        // Waits for the browser callback, validates it, and exchanges its code
        // for tokens. This may be called only once for a login attempt.
        std::expected<OAuthCredentials, std::string> finish(std::chrono::seconds timeout = std::chrono::minutes(5));

        // Stops a pending login. This is also done automatically by the
        // destructor.
        void cancel();

    private:
        struct Impl;
        explicit OAuthLogin(std::unique_ptr<Impl> implementation);
        std::unique_ptr<Impl> implementation_;
    };

    // CODEX_HOME/auth.json when CODEX_HOME is set, otherwise
    // ~/.codex/auth.json. This is intentionally compatible with the credential
    // file used by Codex.
    std::expected<std::filesystem::path, std::string> defaultOAuthCredentialsPath();

    // A missing file is a normal logged-out state and returns an empty optional.
    std::expected<std::optional<OAuthCredentials>, std::string> loadOAuthCredentials(const std::filesystem::path &path);
    std::expected<std::optional<OAuthCredentials>, std::string> loadOAuthCredentials();

    // Saves the Codex-compatible auth.json atomically with owner-only permissions.
    std::expected<void, std::string> saveOAuthCredentials(const OAuthCredentials &credentials, const std::filesystem::path &path);
    std::expected<void, std::string> saveOAuthCredentials(const OAuthCredentials &credentials);

    // Uses the stored refresh token to obtain a new access token. OpenAI may
    // rotate either of the other tokens; absent values retain their old value.
    std::expected<OAuthCredentials, std::string> refreshOAuthCredentials(const OAuthCredentials &credentials, OAuthOptions options = {});

    // Returns true when a credentials file was deleted and false when the user
    // was already logged out.
    std::expected<bool, std::string> logoutOAuth(const std::filesystem::path &path);
    std::expected<bool, std::string> logoutOAuth();

    // Copies the OAuth fields consumed by CodexApi into an existing config.
    void applyOAuthCredentials(CodexApiConfig &config, const OAuthCredentials &credentials);

} // namespace microcodex
