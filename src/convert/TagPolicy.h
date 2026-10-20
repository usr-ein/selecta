// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QString>
#include <QStringView>

namespace selecta::convert {

struct TagPolicyOptions
{
    // The audio is re-encoded (conversion §9.4, re-encode path): ReplayGain values, encoder fields and iTunes gapless
    // data describe the source encoding and are dropped (D-7).
    bool transcoding = true;
    // D-11: other DJ apps' private data (Serato, Traktor) is someone else's cues and grids; dropped even when the
    // audio is kept as-is. Always dropped when transcoding.
    bool stripDjAppData = true;
    // ENCODEDBY (ID3 TENC), e.g. "Selecta 0.1.0" (D-10). Empty: unset.
    QString encodedBy;
    // ENCODING (ID3 TSSE), e.g. "LAME3.100 -b 320". Empty: unset. Never copied from the source.
    QString encoderSettings;
};

// Builds the model to write from a source model (conversion §9.1, TagPolicy). Drops denied fields (isDeniedTagKey),
// sets ENCODEDBY and ENCODING from the options (marked Provenance::Selecta) and keeps everything else, unknown fields
// included. Pictures pass through unchanged (artwork is processed separately). Pure function.
MetadataModel applyTagPolicy(const MetadataModel &source, const TagPolicyOptions &options);

// The deny-list: true for keys that must not be copied under `options`. Always denied: ENCODEDBY, ENCODING, ENCODER,
// ENCODERSETTINGS, LENGTH (stale TLEN) and iTunes' internal comments (COMMENT:ITUNNORM, ITUNSMPB, ITUNPGAP). When
// transcoding: REPLAYGAIN_*, MP3GAIN_*, iTunNORM-like gain fields. With stripDjAppData (or transcoding): SERATO_*,
// TRAKTOR*, "SERATO …" and similar app-private fields.
bool isDeniedTagKey(QStringView key, const TagPolicyOptions &options);

} // namespace selecta::convert
