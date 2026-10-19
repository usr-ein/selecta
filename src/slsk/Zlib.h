// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayView>

// zlib streams (RFC 1950: header, deflate data, Adler-32), as used by share lists, search results and folder contents
// (protocol §2.4). qUncompress() can't be used: it expects Qt's own 4-byte length header.
//
// Pure and reentrant; safe from any thread.
namespace selecta::slsk::zlib {

enum class Error {
    None,
    Truncated,    // the stream ended before its final block and checksum
    TrailingData, // bytes follow the end of the stream
    TooLarge,     // the inflated data would exceed the caller's cap
    Corrupt,      // not a valid zlib stream (bad header, bad data, checksum mismatch)
};

const char *errorName(Error error);

struct Inflated
{
    QByteArray data; // empty unless error == None
    Error error = Error::None;

    bool ok() const { return error == Error::None; }
};

// Inflates a complete zlib stream. Never produces more than `maxOutput` bytes: a stream that inflates past it fails
// with TooLarge as soon as the cap is crossed, so a "zip bomb" costs at most `maxOutput` bytes of memory.
Inflated inflate(QByteArrayView compressed, qsizetype maxOutput);

// Inflates only the first `prefixBytes` of the stream (fewer if it is shorter), without checking the rest. Used to
// read the username and token of a search result before deciding whether to inflate the whole thing.
Inflated inflatePrefix(QByteArrayView compressed, qsizetype prefixBytes);

// Compresses `raw` into a zlib stream at `level` (0–9; Nicotine+ uses 4).
QByteArray deflate(QByteArrayView raw, int level = 4);

} // namespace selecta::slsk::zlib
