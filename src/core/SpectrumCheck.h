// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "library/InboxStore.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace selecta::audio {
class AnalysisCache;
struct QualityReport;
} // namespace selecta::audio

namespace selecta::core {

// What the spectrum check of a downloaded file found (PRD PREV-9), as an Inbox item keeps it.
struct SpectrumCheck
{
    bool checked = false;
    QStringList findings; // audio::findingName()s, in the report's order
    std::optional<int> cutoffHz;
    std::optional<int> likelySourceKbps;
    QString reportKey; // the AnalysisCache key (hex) of the full report with its spectrogram; empty when not cached
};

// Checks a downloaded file: audio::analyzeQuality() over all of it, with the spectrogram when there is a cache to keep
// it in. The report goes into the cache's quality tier under the file's content key, so the inspector can show it after
// the file is converted and gone; the same file checked again is taken from there. The findings say "likely …": they
// are heuristics (old recordings and dark masters can look band-limited) that inform; they don't change what the Inbox
// gets.
//
// Blocking (one decode of the file, about 0.1–0.5 s for six minutes): call it from a worker. Reentrant; `cache` may be
// null and is thread-safe otherwise. A file that can't be analysed, or a cancelled check, gives `checked` false: the
// arrival's own reading and conversion say what is wrong with the file. A report the cache can't store is still used,
// without a key.
SpectrumCheck checkSpectrum(const QString &path, audio::AnalysisCache *cache, const audio::CancellationFlag *cancel);
// The facts of a report, under `reportKey`. Pure.
SpectrumCheck spectrumCheckOf(const audio::QualityReport &report, const QString &reportKey = {});
// Copies a check into an Inbox item's quality fields.
void applySpectrumCheck(const SpectrumCheck &check, library::InboxItem &item);
// The badge for what an item's check found: its first finding as audio::findingBadge() words it ("Likely from a ≈ 192
// kbps MP3"); empty when the check found nothing or didn't run. Names this version doesn't know are skipped. Pure.
QString spectrumBadge(const library::InboxItem &item);

} // namespace selecta::core
