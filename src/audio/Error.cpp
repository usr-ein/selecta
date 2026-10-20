// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/Error.h"

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcAudio, "selecta.audio")

namespace selecta::audio {

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None:
        return u"None"_s;
    case ErrorCode::OpenFailed:
        return u"OpenFailed"_s;
    case ErrorCode::ReadFailed:
        return u"ReadFailed"_s;
    case ErrorCode::WriteFailed:
        return u"WriteFailed"_s;
    case ErrorCode::DiskFull:
        return u"DiskFull"_s;
    case ErrorCode::OutputExists:
        return u"OutputExists"_s;
    case ErrorCode::Unsupported:
        return u"Unsupported"_s;
    case ErrorCode::NeedsCompleteFile:
        return u"NeedsCompleteFile"_s;
    case ErrorCode::SourceFailed:
        return u"SourceFailed"_s;
    case ErrorCode::Corrupt:
        return u"Corrupt"_s;
    case ErrorCode::TooLarge:
        return u"TooLarge"_s;
    case ErrorCode::DecoderError:
        return u"DecoderError"_s;
    case ErrorCode::EncoderError:
        return u"EncoderError"_s;
    case ErrorCode::TagError:
        return u"TagError"_s;
    case ErrorCode::VerificationFailed:
        return u"VerificationFailed"_s;
    case ErrorCode::Cancelled:
        return u"Cancelled"_s;
    case ErrorCode::InvalidArgument:
        return u"InvalidArgument"_s;
    case ErrorCode::Internal:
        return u"Internal"_s;
    }
    return u"Unknown"_s;
}

QString Error::toString() const
{
    if (message.isEmpty()) {
        return errorCodeName(code);
    }
    return errorCodeName(code) + u": "_s + message;
}

} // namespace selecta::audio
