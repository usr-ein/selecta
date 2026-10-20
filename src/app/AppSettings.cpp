// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/AppSettings.h"

#include "base/Paths.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

const QString kAppearance = u"appearance"_s;
const QString kGeometry = u"window/geometry"_s;
const QString kSplitterPrefix = u"window/splitter/"_s;
const QString kSidebar = u"window/sidebarShown"_s;
const QString kInspector = u"window/inspectorShown"_s;
const QString kLastView = u"window/lastView"_s;
const QString kNetworkPaused = u"network/paused"_s;
const QString kPreviewVolume = u"player/volume"_s;
const QString kPreviewSource = u"player/source"_s;
const QString kPreviewFollows = u"player/followSelection"_s;
const QString kYtDlpPath = u"player/ytDlp"_s;
const QString kShowNotifications = u"notifications/show"_s;
const QString kSpotifyTermsAccepted = u"spotify/termsAccepted"_s;
const QString kRecentSearchesKey = u"search/recent"_s;
const QString kConverterTarget = u"converter/target"_s;
const QString kConverterSampleRateKey = u"converter/sampleRate"_s;
const QString kConverterBitDepthKey = u"converter/bitDepth"_s;
const QString kConverterMp3SampleRateKey = u"converter/mp3SampleRate"_s;
const QString kConverterId3v24 = u"converter/id3v24"_s;
const QString kConverterArtwork = u"converter/artworkMax"_s;
const QString kConverterFolder = u"converter/outputFolder"_s;
const QString kInboxDir = u"library/inbox"_s;
const QString kCollectionRoots = u"library/collections"_s;
const QString kSinglesFolder = u"library/singlesFolder"_s;
const QString kAlbumsFolder = u"library/albumsFolder"_s;
const QString kSingleTemplate = u"filing/single"_s;
const QString kAlbumFolderTemplate = u"filing/albumFolder"_s;
const QString kAlbumTrackTemplate = u"filing/albumTrack"_s;
const QString kCleanNames = u"filing/clean"_s;
const QString kSeparator = u"filing/separator"_s;
const QString kServerHost = u"soulseek/host"_s;
const QString kServerPort = u"soulseek/port"_s;
const QString kUsername = u"soulseek/username"_s;
const QString kListenPort = u"soulseek/listenPort"_s;
const QString kConnectAtLaunch = u"soulseek/connectAtLaunch"_s;
const QString kLiveAllowed = u"soulseek/liveAllowed"_s;
const QString kSearchPreset = u"search/presetId"_s;
const QString kSearchCustomised = u"search/customised"_s;
const QString kSearchGrouped = u"search/grouped"_s;
const QString kSearchFiltersShown = u"search/filtersShown"_s;
const QString kSearchFilter = u"search/filter"_s; // JSON (core::filterToJson)
const QString kUserPresets = u"presets/user"_s;   // JSON: [{id, name, filter}]
const QString kDefaultPreset = u"presets/default"_s;
const QString kGetTracksPreset = u"presets/getTracks"_s;
constexpr int kMaxUserPresets = 50;

quint16 portFrom(const QVariant &value, quint16 fallback)
{
    bool ok = false;
    const int port = value.toInt(&ok);
    return ok && port >= 0 && port <= 65535 ? static_cast<quint16>(port) : fallback;
}

// A folder name typed in Settings: one plain component, or the default.
QString folderName(const QString &value, const QString &fallback)
{
    QString name = value.trimmed();
    if (name.isEmpty() || name == u"." || name == u".." || name.contains(u'/') || name.contains(u'\\')) {
        return fallback;
    }
    return name;
}

} // namespace

AppSettings::AppSettings(const QString &iniPath)
    : m_settings(iniPath, QSettings::IniFormat)
{}

Appearance AppSettings::appearance() const
{
    return appearanceFromKey(m_settings.value(kAppearance).toString());
}

void AppSettings::setAppearance(Appearance appearance)
{
    m_settings.setValue(kAppearance, appearanceKey(appearance));
}

QByteArray AppSettings::windowGeometry() const
{
    return m_settings.value(kGeometry).toByteArray();
}

void AppSettings::setWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue(kGeometry, geometry);
}

QByteArray AppSettings::splitterState(const QString &name) const
{
    return m_settings.value(kSplitterPrefix + name).toByteArray();
}

void AppSettings::setSplitterState(const QString &name, const QByteArray &state)
{
    m_settings.setValue(kSplitterPrefix + name, state);
}

bool AppSettings::sidebarShown() const
{
    return m_settings.value(kSidebar, true).toBool();
}

void AppSettings::setSidebarShown(bool shown)
{
    m_settings.setValue(kSidebar, shown);
}

bool AppSettings::inspectorShown() const
{
    return m_settings.value(kInspector, true).toBool();
}

void AppSettings::setInspectorShown(bool shown)
{
    m_settings.setValue(kInspector, shown);
}

QString AppSettings::lastView() const
{
    return m_settings.value(kLastView).toString();
}

void AppSettings::setLastView(const QString &id)
{
    m_settings.setValue(kLastView, id);
}

bool AppSettings::networkPaused() const
{
    return m_settings.value(kNetworkPaused, false).toBool();
}

void AppSettings::setNetworkPaused(bool paused)
{
    m_settings.setValue(kNetworkPaused, paused);
}

double AppSettings::previewVolume() const
{
    bool ok = false;
    const double volume = m_settings.value(kPreviewVolume, 0.8).toDouble(&ok);
    return ok && std::isfinite(volume) ? std::clamp(volume, 0.0, 1.0) : 0.8;
}

void AppSettings::setPreviewVolume(double volume)
{
    m_settings.setValue(kPreviewVolume, std::clamp(volume, 0.0, 1.0));
}

core::PreviewSource AppSettings::previewSource() const
{
    const QString source = m_settings.value(kPreviewSource).toString();
    if (source == u"youtube"_s) {
        return core::PreviewSource::YouTube;
    }
    if (source == u"clip"_s) {
        return core::PreviewSource::Clip;
    }
    if (source == u"peer"_s) {
        return core::PreviewSource::Peer;
    }
    return core::PreviewSource::Automatic;
}

void AppSettings::setPreviewSource(core::PreviewSource source)
{
    switch (source) {
    case core::PreviewSource::YouTube:
        m_settings.setValue(kPreviewSource, u"youtube"_s);
        return;
    case core::PreviewSource::Clip:
        m_settings.setValue(kPreviewSource, u"clip"_s);
        return;
    case core::PreviewSource::Peer:
        m_settings.setValue(kPreviewSource, u"peer"_s);
        return;
    case core::PreviewSource::Automatic:
        break;
    }
    m_settings.remove(kPreviewSource);
}

bool AppSettings::previewFollowsSelection() const
{
    return m_settings.value(kPreviewFollows, false).toBool();
}

void AppSettings::setPreviewFollowsSelection(bool follows)
{
    m_settings.setValue(kPreviewFollows, follows);
}

bool AppSettings::showNotifications() const
{
    return m_settings.value(kShowNotifications, true).toBool();
}

void AppSettings::setShowNotifications(bool show)
{
    m_settings.setValue(kShowNotifications, show);
}

QString AppSettings::ytDlpPath() const
{
    return m_settings.value(kYtDlpPath).toString().trimmed();
}

QStringList AppSettings::recentSearches() const
{
    return m_settings.value(kRecentSearchesKey).toStringList().mid(0, kRecentSearches);
}

void AppSettings::addRecentSearch(const QString &text)
{
    const QString cleaned = text.simplified();
    if (cleaned.isEmpty()) {
        return;
    }
    QStringList searches = recentSearches();
    searches.removeIf([&cleaned](const QString &search) { return search.compare(cleaned, Qt::CaseInsensitive) == 0; });
    searches.prepend(cleaned);
    m_settings.setValue(kRecentSearchesKey, searches.mid(0, kRecentSearches));
}

AppSettings::ConverterState AppSettings::converterState() const
{
    ConverterState state;
    static const QStringList kTargets{u"mp3-320"_s, u"mp3-256"_s, u"mp3-192"_s, u"mp3-v0"_s, u"mp3-v2"_s,
                                      u"aiff"_s,    u"wav"_s,     u"flac"_s,    u"alac"_s};
    const QString target = m_settings.value(kConverterTarget).toString();
    if (kTargets.contains(target)) {
        state.target = target;
    }
    bool ok = false;
    const int sampleRate = m_settings.value(kConverterSampleRateKey).toInt(&ok);
    if (ok && std::ranges::find(kConverterSampleRates, sampleRate) != kConverterSampleRates.end()) {
        state.sampleRate = sampleRate;
    }
    const int bitDepth = m_settings.value(kConverterBitDepthKey).toInt(&ok);
    if (ok && std::ranges::find(kConverterBitDepths, bitDepth) != kConverterBitDepths.end()) {
        state.bitDepth = bitDepth;
    }
    const int mp3SampleRate = m_settings.value(kConverterMp3SampleRateKey).toInt(&ok);
    if (ok && std::ranges::find(kConverterMp3SampleRates, mp3SampleRate) != kConverterMp3SampleRates.end()) {
        state.mp3SampleRate = mp3SampleRate;
    }
    state.id3v24 = m_settings.value(kConverterId3v24, false).toBool();
    const int artwork = m_settings.value(kConverterArtwork).toInt(&ok);
    if (ok && artwork >= 0 && artwork <= 3000) {
        state.artworkMax = artwork;
    }
    state.outputFolder = m_settings.value(kConverterFolder).toString();
    return state;
}

void AppSettings::setConverterState(const ConverterState &state)
{
    m_settings.setValue(kConverterTarget, state.target);
    m_settings.setValue(kConverterSampleRateKey, state.sampleRate);
    m_settings.setValue(kConverterBitDepthKey, state.bitDepth);
    m_settings.setValue(kConverterMp3SampleRateKey, state.mp3SampleRate);
    m_settings.setValue(kConverterId3v24, state.id3v24);
    m_settings.setValue(kConverterArtwork, state.artworkMax);
    if (state.outputFolder.isEmpty()) {
        m_settings.remove(kConverterFolder);
    } else {
        m_settings.setValue(kConverterFolder, state.outputFolder);
    }
}

void AppSettings::setYtDlpPath(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        m_settings.remove(kYtDlpPath);
    } else {
        m_settings.setValue(kYtDlpPath, path.trimmed());
    }
}

bool AppSettings::spotifyTermsAccepted() const
{
    return m_settings.value(kSpotifyTermsAccepted, false).toBool();
}

void AppSettings::setSpotifyTermsAccepted(bool accepted)
{
    if (accepted) {
        m_settings.setValue(kSpotifyTermsAccepted, true);
    } else {
        m_settings.remove(kSpotifyTermsAccepted);
    }
}

core::LibraryConfig AppSettings::libraryConfig() const
{
    core::LibraryConfig config;
    const QString inbox = m_settings.value(kInboxDir).toString();
    config.inboxRoot = inbox.isEmpty() ? base::paths::defaultInboxDir() : inbox;
    config.collectionRoots = m_settings.value(kCollectionRoots).toStringList();
    config.collectionRoots.removeAll(QString());
    config.singlesFolder = folderName(m_settings.value(kSinglesFolder).toString(), config.singlesFolder);
    config.albumsFolder = folderName(m_settings.value(kAlbumsFolder).toString(), config.albumsFolder);
    const auto templ = [this](const QString &key, const QString &fallback) {
        const QString value = m_settings.value(key).toString().trimmed();
        return value.isEmpty() ? fallback : value;
    };
    config.templates.single = templ(kSingleTemplate, config.templates.single);
    config.templates.albumFolder = templ(kAlbumFolderTemplate, config.templates.albumFolder);
    config.templates.albumTrack = templ(kAlbumTrackTemplate, config.templates.albumTrack);
    config.cleaner.enabled = m_settings.value(kCleanNames, true).toBool();
    const QString separator = m_settings.value(kSeparator).toString();
    if (separator.size() == 1 && !separator.at(0).isLetterOrNumber() && separator != u"/" && separator != u"\\") {
        config.cleaner.separator = separator.at(0);
    }
    return config;
}

void AppSettings::setLibraryConfig(const core::LibraryConfig &config)
{
    m_settings.setValue(kInboxDir, config.inboxRoot);
    m_settings.setValue(kCollectionRoots, config.collectionRoots);
    m_settings.setValue(kSinglesFolder, config.singlesFolder);
    m_settings.setValue(kAlbumsFolder, config.albumsFolder);
    m_settings.setValue(kSingleTemplate, config.templates.single);
    m_settings.setValue(kAlbumFolderTemplate, config.templates.albumFolder);
    m_settings.setValue(kAlbumTrackTemplate, config.templates.albumTrack);
    m_settings.setValue(kCleanNames, config.cleaner.enabled);
    m_settings.setValue(kSeparator, QString(config.cleaner.separator));
}

core::NetworkConfig AppSettings::networkConfig() const
{
    core::NetworkConfig config;
    const QString host = m_settings.value(kServerHost).toString().trimmed();
    if (!host.isEmpty()) {
        config.host = host;
    }
    config.port = portFrom(m_settings.value(kServerPort), config.port);
    config.username = m_settings.value(kUsername).toString().trimmed();
    config.listenPort = portFrom(m_settings.value(kListenPort), config.listenPort);
    config.connectAtLaunch = m_settings.value(kConnectAtLaunch, config.connectAtLaunch).toBool();
    config.liveAllowedByUser = m_settings.value(kLiveAllowed, false).toBool();
    return config;
}

void AppSettings::setNetworkConfig(const core::NetworkConfig &config)
{
    m_settings.setValue(kServerHost, config.host);
    m_settings.setValue(kServerPort, config.port);
    m_settings.setValue(kUsername, config.username);
    m_settings.setValue(kListenPort, config.listenPort);
    m_settings.setValue(kConnectAtLaunch, config.connectAtLaunch);
    m_settings.setValue(kLiveAllowed, config.liveAllowedByUser);
}

AppSettings::SearchViewState AppSettings::searchViewState() const
{
    SearchViewState state;
    const QString presetId = m_settings.value(kSearchPreset, defaultPresetId()).toString();
    const core::FilterPreset preset = presetById(presetId).value_or(core::builtInPresets().constFirst());
    state.presetId = preset.id;
    state.filter = preset.filter;
    state.grouped = m_settings.value(kSearchGrouped, false).toBool();
    state.filtersShown = m_settings.value(kSearchFiltersShown, false).toBool();
    state.customised = m_settings.value(kSearchCustomised, false).toBool();
    const QJsonDocument saved = QJsonDocument::fromJson(m_settings.value(kSearchFilter).toString().toUtf8());
    if (saved.isObject()) {
        const core::SearchFilter filter = core::filterFromJson(saved.object());
        if (state.customised) {
            state.filter = filter;
        } else {
            // Not part of the quality: kept whichever preset is chosen.
            state.filter.onlyThisRelease = filter.onlyThisRelease;
            state.filter.owned = filter.owned;
        }
    }
    return state;
}

void AppSettings::setSearchViewState(const SearchViewState &state)
{
    m_settings.setValue(kSearchPreset, state.presetId);
    m_settings.setValue(kSearchCustomised, state.customised);
    m_settings.setValue(kSearchGrouped, state.grouped);
    m_settings.setValue(kSearchFiltersShown, state.filtersShown);
    m_settings.setValue(
        kSearchFilter,
        QString::fromUtf8(QJsonDocument(core::filterToJson(state.filter)).toJson(QJsonDocument::Compact)));
}

QList<core::FilterPreset> AppSettings::userPresets() const
{
    QList<core::FilterPreset> presets;
    const QJsonArray saved = QJsonDocument::fromJson(m_settings.value(kUserPresets).toString().toUtf8()).array();
    for (const auto &value : saved) {
        const QJsonObject object = value.toObject();
        const QString id = object.value("id"_L1).toString();
        const QString name = object.value("name"_L1).toString().trimmed();
        if (!id.startsWith(u"user-"_s) || name.isEmpty() || presets.size() >= kMaxUserPresets) {
            continue;
        }
        presets.append(core::FilterPreset{.id = id,
                                          .name = name.left(80),
                                          .filter = core::filterFromJson(object.value("filter"_L1).toObject()),
                                          .builtIn = false});
    }
    return presets;
}

void AppSettings::setUserPresets(const QList<core::FilterPreset> &presets)
{
    QJsonArray saved;
    for (const core::FilterPreset &preset : presets) {
        if (!preset.builtIn) {
            saved.append(QJsonObject{
                {"id"_L1, preset.id}, {"name"_L1, preset.name}, {"filter"_L1, core::filterToJson(preset.filter)}});
        }
    }
    m_settings.setValue(kUserPresets, QString::fromUtf8(QJsonDocument(saved).toJson(QJsonDocument::Compact)));
}

QString AppSettings::defaultPresetId() const
{
    const QString id = m_settings.value(kDefaultPreset).toString();
    return presetById(id) ? id : core::kMp3OrLosslessPreset.toString();
}

void AppSettings::setDefaultPresetId(const QString &id)
{
    m_settings.setValue(kDefaultPreset, id);
}

QString AppSettings::getTracksPresetId() const
{
    const QString id = m_settings.value(kGetTracksPreset).toString();
    return presetById(id) ? id : defaultPresetId();
}

void AppSettings::setGetTracksPresetId(const QString &id)
{
    m_settings.setValue(kGetTracksPreset, id);
}

QList<core::FilterPreset> AppSettings::presets() const
{
    return core::builtInPresets() + userPresets();
}

std::optional<core::FilterPreset> AppSettings::presetById(const QString &id) const
{
    if (std::optional<core::FilterPreset> builtIn = core::builtInPreset(id)) {
        return builtIn;
    }
    const QList<core::FilterPreset> mine = userPresets();
    for (const core::FilterPreset &preset : mine) {
        if (preset.id == id) {
            return preset;
        }
    }
    return std::nullopt;
}

bool AppSettings::sync()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

} // namespace selecta::app
