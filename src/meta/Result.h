// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"

#include <QMetaType>
#include <QString>

#include <chrono>
#include <optional>
#include <utility>
#include <variant>

// The error model shared by every meta component: an operation either produces its value or a MetaError, never
// both, never neither. Asynchronous operations return QFuture<Result<T>>; the future is only ever *canceled* when the
// caller cancels it, so a finished future always holds a Result.
namespace selecta::meta {

// The outside services meta talks to. Each one has its own pacing (ServicePolicy.h) and consumes one kind of
// governor permit (operationFor()).
enum class Service : quint8 {
    SpotifyApi,      // api.spotify.com: catalogue, playlists, Liked Songs
    SpotifyAccounts, // accounts.spotify.com: sign-in and tokens
    SpotifyImages,   // i.scdn.co: cover images linked from Spotify
    Deezer,          // api.deezer.com: keyless catalogue and 30-second clip URLs
    MusicBrainz,     // musicbrainz.org/ws/2
    CoverArtArchive, // coverartarchive.org and the archive.org hosts it redirects to
    YtDlp,           // the user's own yt-dlp, which talks to YouTube
    Keychain,        // local secret storage (TokenStore): not a network service, never asks for a permit
};

// The governor permit a request to `service` consumes; none for Keychain. Thread-safe (pure).
std::optional<governor::Operation> operationFor(Service service);
// "Spotify", "MusicBrainz", … for messages and logs. Thread-safe (pure).
QString serviceName(Service service);

enum class ErrorCode : quint8 {
    // Configuration and sign-in
    NotConfigured,  // a required setting is missing: no Spotify Client ID, no yt-dlp, no cache folder…
    SignInRequired, // not signed in to Spotify, or the sign-in expired (6-month refresh token) or was revoked
    MissingScope,   // signed in, but without the permission this call needs: sign in again for it
    AccessDenied,   // the user declined in the browser
    Forbidden,      // HTTP 403: account not on the app's user list, playlist neither owned nor collaborative…
    StateMismatch,  // a sign-in answer that doesn't belong to the sign-in in progress
    PortUnavailable, // the loopback port for the sign-in answer can't be opened

    // Requests
    InvalidArgument,   // the input can't be sent: malformed link or ID, empty query, bad ISRC
    NotFound,          // the service has no such item, or nothing matched
    RateLimited,       // the service asked Selecta to wait (429, 503, Deezer quota, YouTube); see retryAfter
    QuotaExceeded,     // Spotify's per-developer-account quota (429 with reason QUOTA_EXCEEDED)
    Refused,           // the governor refused the request (network paused, budget used up, shutting down)
    Network,           // DNS, connection or TLS failure
    Timeout,           // no complete answer in time
    ServerError,       // HTTP 5xx, after the retries the service's policy allows
    BadResponse,       // malformed or unexpected answer
    TooLarge,          // an answer or image above its bound (bytes or pixels)
    UnsupportedFormat, // an image format meta doesn't accept

    // yt-dlp (PREV-11)
    ToolNotFound,       // no usable yt-dlp executable
    ToolUnsafe,         // found, but writable by others or owned by someone else: not run
    ToolTooOld,         // older than the minimum version Selecta supports
    ToolMissingRuntime, // yt-dlp needs a JavaScript runtime (Deno) for YouTube and has none
    ToolFailed,         // yt-dlp failed for another reason; the message carries its last error line
    BotCheck,           // YouTube asks for a sign-in or captcha to prove this isn't a bot
    AgeRestricted,      // the video needs a signed-in, age-verified account
    GeoBlocked,         // not available in the user's region
    Unavailable,        // removed, private, DRM-protected or otherwise unplayable

    Cancelled, // the caller cancelled a job (YtDlpFetch); QFuture-based calls end canceled instead
    Internal,  // a bug in Selecta: should never happen
};

// Short English name of an error code ("rate limited"), for logs. Thread-safe (pure).
QString errorCodeName(ErrorCode code);

struct MetaError
{
    ErrorCode code = ErrorCode::Internal;
    Service service = Service::SpotifyApi;
    // English, safe to show and to log: never contains a token, secret, Client ID or authorisation code.
    QString message;
    int httpStatus = 0;                           // the HTTP status that caused it, 0 if none
    std::optional<std::chrono::seconds> retryAfter; // RateLimited / QuotaExceeded: how long the service asked to wait

    // "MusicBrainz: rate limited (HTTP 503): …", for logs.
    QString toString() const;
};

// A value or an error. Copyable when T is. Accessing the wrong alternative throws std::bad_variant_access (and
// asserts in debug builds), so check ok() first.
template <typename T>
class [[nodiscard]] Result
{
public:
    Result(T value) // NOLINT(google-explicit-constructor): a value converts to a successful result
        : m_data(std::in_place_index<0>, std::move(value))
    {}
    Result(MetaError error) // NOLINT(google-explicit-constructor): an error converts to a failed result
        : m_data(std::in_place_index<1>, std::move(error))
    {}

    bool ok() const { return m_data.index() == 0; }
    explicit operator bool() const { return ok(); }

    const T &value() const &
    {
        Q_ASSERT(ok());
        return std::get<0>(m_data);
    }
    T &value() &
    {
        Q_ASSERT(ok());
        return std::get<0>(m_data);
    }
    T &&value() &&
    {
        Q_ASSERT(ok());
        return std::get<0>(std::move(m_data));
    }
    const MetaError &error() const
    {
        Q_ASSERT(!ok());
        return std::get<1>(m_data);
    }

private:
    std::variant<T, MetaError> m_data;
};

// Success without a value, or an error.
template <>
class [[nodiscard]] Result<void>
{
public:
    Result() = default; // success
    Result(MetaError error) // NOLINT(google-explicit-constructor): an error converts to a failed result
        : m_error(std::move(error))
    {}

    bool ok() const { return !m_error.has_value(); }
    explicit operator bool() const { return ok(); }
    const MetaError &error() const
    {
        Q_ASSERT(!ok());
        return m_error.value();
    }

private:
    std::optional<MetaError> m_error;
};

// Builds a MetaError. Thread-safe (pure).
MetaError makeError(Service service, ErrorCode code, QString message, int httpStatus = 0);

} // namespace selecta::meta

Q_DECLARE_METATYPE(selecta::meta::MetaError)
