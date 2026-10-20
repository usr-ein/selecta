// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Presets.h"

#include "app/AppSettings.h"

#include <QSignalSpy>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

class PresetsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        settings = std::make_unique<selecta::app::AppSettings>(dir.filePath(u"settings.ini"_s));
        presets = std::make_unique<selecta::app::Presets>(*settings);
    }

    static selecta::core::SearchFilter djFilter()
    {
        selecta::core::SearchFilter filter;
        filter.setAllowed(selecta::core::SearchFormat::Mp3, true);
        filter.bitrateRule = selecta::core::SearchFilter::BitrateRule::Equal;
        filter.bitrateKbps = 320;
        filter.cbrOnly = true;
        filter.strict = true;
        return filter;
    }

    QTemporaryDir dir;
    std::unique_ptr<selecta::app::AppSettings> settings;
    std::unique_ptr<selecta::app::Presets> presets;
};

} // namespace

TEST_F(PresetsTest, SelectasComeFirstAndTheDefaultIsMp3OrLossless)
{
    const QList<selecta::core::FilterPreset> all = presets->all();
    ASSERT_EQ(all.size(), 4);
    EXPECT_TRUE(all.constFirst().builtIn);
    EXPECT_EQ(presets->defaultId(), selecta::core::kMp3OrLosslessPreset);
    EXPECT_EQ(presets->getTracksId(), selecta::core::kMp3OrLosslessPreset);
    // An unknown id reads as the default.
    EXPECT_EQ(presets->preset(u"user-99"_s).id, selecta::core::kMp3OrLosslessPreset);
}

TEST_F(PresetsTest, SavedPresetsAreKeptRenamedAndDeleted)
{
    const QSignalSpy changed(presets.get(), &selecta::app::Presets::changed);
    const std::optional<QString> id = presets->save(u"  DJ — MP3 320  "_s, djFilter());
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(changed.size(), 1);
    ASSERT_TRUE(presets->byId(*id).has_value());
    EXPECT_EQ(presets->byId(*id)->name, u"DJ — MP3 320"_s);
    EXPECT_EQ(presets->byId(*id)->filter, djFilter());
    EXPECT_FALSE(presets->byId(*id)->builtIn);

    // Saving under the same name (any case) replaces it; Selecta's names can't be taken.
    selecta::core::SearchFilter wider = djFilter();
    wider.cbrOnly = false;
    EXPECT_EQ(presets->save(u"dj — mp3 320"_s, wider), id);
    EXPECT_EQ(presets->byId(*id)->filter, wider);
    EXPECT_FALSE(presets->save(u"Lossless only"_s, wider).has_value());
    EXPECT_FALSE(presets->save(u"   "_s, wider).has_value());

    // They survive a restart (the settings file).
    settings = std::make_unique<selecta::app::AppSettings>(dir.filePath(u"settings.ini"_s));
    presets = std::make_unique<selecta::app::Presets>(*settings);
    ASSERT_EQ(presets->all().size(), 5);
    EXPECT_EQ(presets->all().constLast().id, *id);

    EXPECT_TRUE(presets->rename(*id, u"Club MP3"_s));
    EXPECT_EQ(presets->byId(*id)->name, u"Club MP3"_s);
    EXPECT_FALSE(presets->rename(*id, u"Any quality"_s)); // one of Selecta's
    EXPECT_FALSE(presets->rename(selecta::core::kHiResPreset.toString(), u"Mine now"_s));

    // The default and Get tracks' choice can be the user's; deleting it falls back to Selecta's default.
    presets->setDefaultId(*id);
    presets->setGetTracksId(*id);
    EXPECT_EQ(presets->defaultId(), *id);
    EXPECT_EQ(presets->getTracksId(), *id);
    presets->remove(*id);
    EXPECT_FALSE(presets->byId(*id).has_value());
    EXPECT_EQ(presets->defaultId(), selecta::core::kMp3OrLosslessPreset);
    EXPECT_EQ(presets->getTracksId(), selecta::core::kMp3OrLosslessPreset);
    presets->remove(selecta::core::kLosslessPreset.toString()); // Selecta's stay
    EXPECT_EQ(presets->all().size(), 4);
}

TEST_F(PresetsTest, TheSearchViewRemembersItsPresetAndEdits)
{
    selecta::app::AppSettings::SearchViewState state;
    state.presetId = selecta::core::kLosslessPreset.toString();
    state.filter = presets->preset(state.presetId).filter;
    state.filter.onlyThisRelease = false;
    settings->setSearchViewState(state);
    EXPECT_EQ(settings->searchViewState().presetId, selecta::core::kLosslessPreset);
    EXPECT_FALSE(settings->searchViewState().filter.onlyThisRelease); // kept, whatever the preset

    state.customised = true;
    state.filter.minSpeedKBps = 250;
    settings->setSearchViewState(state);
    EXPECT_TRUE(settings->searchViewState().customised);
    EXPECT_EQ(settings->searchViewState().filter, state.filter);
}
