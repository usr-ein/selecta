// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SpectrumCheck.h"

#include "audio/AnalysisCache.h"
#include "audio/ByteSource.h"
#include "audio/QualityCheck.h"

#include <QLoggingCategory>

#include <memory>

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcSpectrum, "selecta.core.spectrum")

} // namespace

SpectrumCheck spectrumCheckOf(const audio::QualityReport &report, const QString &reportKey)
{
    SpectrumCheck check;
    check.checked = true;
    for (const audio::QualityFinding finding : report.findings) {
        check.findings.append(audio::findingName(finding));
    }
    if (report.cutoffHz > 0) {
        check.cutoffHz = report.cutoffHz;
    }
    check.likelySourceKbps = report.likelySourceKbps;
    check.reportKey = reportKey;
    return check;
}

SpectrumCheck checkSpectrum(const QString &path, audio::AnalysisCache *cache, const audio::CancellationFlag *cancel)
{
    audio::Result<std::shared_ptr<audio::FileSource>> opened = audio::FileSource::open(path);
    if (!opened) {
        qCInfo(lcSpectrum).noquote() << "not checked:" << opened.error().toString();
        return {};
    }
    const std::shared_ptr<audio::FileSource> source = opened.takeValue();
    std::optional<audio::CacheKey> key;
    if (cache != nullptr) {
        if (const audio::Result<audio::CacheKey> content = audio::CacheKey::forContent(*source); content) {
            key = content.value();
            // The same file again (downloaded twice, or an arrival that ran before a quit): checked already.
            if (const std::optional<audio::QualityReport> cached = cache->loadQuality(*key);
                cached && cached->spectrogram) {
                return spectrumCheckOf(*cached, key->toHex());
            }
        } else {
            qCInfo(lcSpectrum).noquote() << "no cache key:" << content.error().toString();
        }
    }
    audio::QualityOptions options;
    options.spectrogram = key.has_value(); // only worth the extra FFTs when it can be kept
    const audio::Result<audio::QualityReport> analysed = audio::analyzeQuality(source, options, cancel);
    if (!analysed) {
        if (analysed.error().code != audio::ErrorCode::Cancelled) {
            qCInfo(lcSpectrum).noquote() << "not checked:" << analysed.error().toString();
        }
        return {};
    }
    QString stored;
    if (key) {
        if (const audio::Result<void> kept = cache->storeQuality(*key, analysed.value()); kept) {
            stored = key->toHex();
        } else {
            qCWarning(lcSpectrum).noquote() << "the spectrogram isn't kept:" << kept.error().toString();
        }
    }
    return spectrumCheckOf(analysed.value(), stored);
}

QString spectrumBadge(const library::InboxItem &item)
{
    for (const QString &name : item.qualityFindings) {
        if (const std::optional<audio::QualityFinding> finding = audio::findingFromName(name)) {
            return audio::findingBadge(*finding, item.qualitySourceKbps);
        }
    }
    return {};
}

void applySpectrumCheck(const SpectrumCheck &check, library::InboxItem &item)
{
    item.qualityChecked = check.checked;
    item.qualityFindings = check.findings;
    item.qualityCutoffHz = check.cutoffHz;
    item.qualitySourceKbps = check.likelySourceKbps;
    item.qualityKey = check.reportKey;
}

} // namespace selecta::core
