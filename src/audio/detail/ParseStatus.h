// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QString>

namespace selecta::audio::detail {

// Outcome of parsing a header from a (possibly growing) prefix of a file.
enum class ParseStatus {
    Ok,
    NeedMoreData,      // the prefix ends before the header does; retry when the source grows
    NeedsCompleteFile, // this layout can only be parsed from the complete file (e.g. AIFF with COMM after SSND)
    Unsupported,       // a variant Selecta's own parser doesn't handle; FFmpeg may, for complete files
    Corrupt,
    TooLarge, // a header bound was exceeded
    ReadError,
};

struct ParseResult
{
    ParseStatus status = ParseStatus::Ok;
    QString message; // why, for anything but Ok and NeedMoreData

    bool ok() const { return status == ParseStatus::Ok; }
    static ParseResult success() { return {}; }
    static ParseResult needMoreData() { return {ParseStatus::NeedMoreData, {}}; }
    static ParseResult failure(ParseStatus status, QString message) { return {status, std::move(message)}; }
};

inline ErrorCode errorCodeFor(ParseStatus status)
{
    switch (status) {
    case ParseStatus::Ok:
    case ParseStatus::NeedMoreData:
        return ErrorCode::None;
    case ParseStatus::NeedsCompleteFile:
        return ErrorCode::NeedsCompleteFile;
    case ParseStatus::Unsupported:
        return ErrorCode::Unsupported;
    case ParseStatus::Corrupt:
        return ErrorCode::Corrupt;
    case ParseStatus::TooLarge:
        return ErrorCode::TooLarge;
    case ParseStatus::ReadError:
        return ErrorCode::ReadFailed;
    }
    return ErrorCode::Internal;
}

} // namespace selecta::audio::detail
