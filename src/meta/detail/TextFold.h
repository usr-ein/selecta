// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// The comparison key of references/library-matching-and-organization.md §4.2, steps 1–6: NFKD, diacritics dropped,
// case folded, a fixed transliteration table for letters that don't decompose (ø→o, æ→ae, œ→oe, ß→ss, ł→l, đ/ð→d,
// þ→th, ı→i; the same on every platform), & and + → "and", $ → s, apostrophes removed without a space, dots between
// single letters removed (A.T.F.C. → atfc), every other non-alphanumeric → space, single spaces, trimmed. Bounded:
// input beyond 1000 characters is ignored. Pure, thread-safe.
namespace selecta::meta::detail {

QString foldKey(const QString &input);

// The key without spaces ("daft punk" ≡ "daftpunk", "ac dc" ≡ "acdc").
QString squash(const QString &key);

} // namespace selecta::meta::detail
