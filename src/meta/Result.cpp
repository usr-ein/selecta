// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/Result.h"

using namespace Qt::StringLiterals;

namespace selecta::meta {

std::optional<governor::Operation> operationFor(Service service)
{
    using governor::Operation;
    switch (service) {
    case Service::SpotifyApi:
    case Service::SpotifyAccounts:
        return Operation::SpotifyRequest;
    case Service::SpotifyImages:
    case Service::CoverArtArchive:
        return Operation::CoverArtRequest;
    case Service::Deezer:
        return Operation::DeezerRequest;
    case Service::MusicBrainz:
        return Operation::MusicBrainzRequest;
    case Service::YtDlp:
        return Operation::YtDlpRun;
    case Service::Keychain:
        return std::nullopt;
    }
    return std::nullopt;
}

QString serviceName(Service service)
{
    switch (service) {
    case Service::SpotifyApi:
        return u"Spotify"_s;
    case Service::SpotifyAccounts:
        return u"Spotify accounts"_s;
    case Service::SpotifyImages:
        return u"Spotify images"_s;
    case Service::Deezer:
        return u"Deezer"_s;
    case Service::MusicBrainz:
        return u"MusicBrainz"_s;
    case Service::CoverArtArchive:
        return u"Cover Art Archive"_s;
    case Service::YtDlp:
        return u"yt-dlp"_s;
    case Service::Keychain:
        return u"Keychain"_s;
    }
    return u"unknown service"_s;
}

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::NotConfigured:
        return u"not configured"_s;
    case ErrorCode::SignInRequired:
        return u"sign-in required"_s;
    case ErrorCode::MissingScope:
        return u"missing permission"_s;
    case ErrorCode::AccessDenied:
        return u"access denied"_s;
    case ErrorCode::Forbidden:
        return u"forbidden"_s;
    case ErrorCode::StateMismatch:
        return u"state mismatch"_s;
    case ErrorCode::PortUnavailable:
        return u"port unavailable"_s;
    case ErrorCode::InvalidArgument:
        return u"invalid argument"_s;
    case ErrorCode::NotFound:
        return u"not found"_s;
    case ErrorCode::RateLimited:
        return u"rate limited"_s;
    case ErrorCode::QuotaExceeded:
        return u"quota exceeded"_s;
    case ErrorCode::Refused:
        return u"refused by the governor"_s;
    case ErrorCode::Network:
        return u"network error"_s;
    case ErrorCode::Timeout:
        return u"timeout"_s;
    case ErrorCode::ServerError:
        return u"server error"_s;
    case ErrorCode::BadResponse:
        return u"bad response"_s;
    case ErrorCode::TooLarge:
        return u"too large"_s;
    case ErrorCode::UnsupportedFormat:
        return u"unsupported format"_s;
    case ErrorCode::ToolNotFound:
        return u"yt-dlp not found"_s;
    case ErrorCode::ToolUnsafe:
        return u"yt-dlp unsafe"_s;
    case ErrorCode::ToolTooOld:
        return u"yt-dlp too old"_s;
    case ErrorCode::ToolMissingRuntime:
        return u"JavaScript runtime missing"_s;
    case ErrorCode::ToolFailed:
        return u"yt-dlp failed"_s;
    case ErrorCode::BotCheck:
        return u"bot check"_s;
    case ErrorCode::AgeRestricted:
        return u"age-restricted"_s;
    case ErrorCode::GeoBlocked:
        return u"not available in this region"_s;
    case ErrorCode::Unavailable:
        return u"unavailable"_s;
    case ErrorCode::Cancelled:
        return u"cancelled"_s;
    case ErrorCode::Internal:
        return u"internal error"_s;
    }
    return u"unknown error"_s;
}

QString MetaError::toString() const
{
    QString text = serviceName(service) + u": "_s + errorCodeName(code);
    if (httpStatus != 0) {
        text += u" (HTTP %1)"_s.arg(httpStatus);
    }
    if (retryAfter.has_value()) {
        text += u", retry after %1 s"_s.arg(retryAfter->count());
    }
    if (!message.isEmpty()) {
        text += u": "_s + message;
    }
    return text;
}

MetaError makeError(Service service, ErrorCode code, QString message, int httpStatus)
{
    MetaError error;
    error.code = code;
    error.service = service;
    error.message = std::move(message);
    error.httpStatus = httpStatus;
    return error;
}

} // namespace selecta::meta
