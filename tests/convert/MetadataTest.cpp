// SPDX-License-Identifier: GPL-3.0-or-later
// C1: the metadata model, the alias table and the tag policy's deny-list.
#include "convert/MetadataModel.h"
#include "convert/TagPolicy.h"
#include "convert/TagReader.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;

TEST(MetadataModel, KeysValuesAndProvenance)
{
    MetadataModel model;
    EXPECT_TRUE(model.isEmpty());
    model.set(u"title", u"Song"_s, Provenance::Embedded);
    EXPECT_TRUE(model.contains(keys::Title)); // keys are upper case
    EXPECT_EQ(model.value(u"TITLE"), u"Song"_s);
    EXPECT_EQ(model.provenance(keys::Title), Provenance::Embedded);
    model.set(keys::Artist, QStringList{u"A"_s, QString(), u"B"_s}, Provenance::Release);
    EXPECT_EQ(model.values(keys::Artist), (QStringList{u"A"_s, u"B"_s})); // empty values dropped
    model.set(keys::Artist, QStringList{QString()}, Provenance::Release);
    EXPECT_FALSE(model.contains(keys::Artist)); // nothing left removes the field
    model.remove(keys::Title);
    EXPECT_TRUE(model.isEmpty());
    EXPECT_EQ(model.value(keys::Album), QString());
    EXPECT_EQ(model.provenance(keys::Album), Provenance::Unknown);
    model.set(QStringView(), u"x"_s, Provenance::User); // an empty key is ignored
    EXPECT_TRUE(model.isEmpty());
}

TEST(MetadataModel, PicturesAndFrontCover)
{
    MetadataModel model;
    EXPECT_FALSE(model.frontCover());
    Picture back;
    back.type = Picture::kTypeBackCover;
    back.data = "back";
    Picture front;
    front.data = "front";
    model.setPictures({back, front});
    ASSERT_TRUE(model.frontCover());
    EXPECT_EQ(model.frontCover()->data, "front");
    Picture other;
    other.type = Picture::kTypeOther;
    other.data = "new";
    model.setFrontCover(other); // replaces everything, forced to front cover
    ASSERT_EQ(model.pictures().size(), 1);
    EXPECT_TRUE(model.pictures().first().isFrontCover());
    model.clearPictures();
    EXPECT_TRUE(model.pictures().isEmpty());
}

TEST(MetadataModel, SupplementAndOverride)
{
    MetadataModel file;
    file.set(keys::Title, u"file title"_s, Provenance::Embedded);
    file.set(keys::Genre, u"House"_s, Provenance::Embedded);
    MetadataModel release;
    release.set(keys::Title, u"release title"_s, Provenance::Release);
    release.set(keys::Isrc, u"XX0000000001"_s, Provenance::Release);
    Picture cover;
    cover.data = "cover";
    release.setFrontCover(cover);

    MetadataModel supplemented = file;
    supplemented.supplementFrom(release);
    EXPECT_EQ(supplemented.value(keys::Title), u"file title"_s);
    EXPECT_EQ(supplemented.value(keys::Isrc), u"XX0000000001"_s);
    EXPECT_EQ(supplemented.provenance(keys::Isrc), Provenance::Release);
    EXPECT_TRUE(supplemented.frontCover());

    MetadataModel overridden = file;
    overridden.overrideFrom(release);
    EXPECT_EQ(overridden.value(keys::Title), u"release title"_s);
    EXPECT_EQ(overridden.value(keys::Genre), u"House"_s);
    EXPECT_EQ(overridden.provenance(keys::Title), Provenance::Release);
}

TEST(TagAliases, CanonicalKeys)
{
    EXPECT_EQ(canonicalTagKey(u"album artist"), u"ALBUMARTIST"_s);
    EXPECT_EQ(canonicalTagKey(u"ALBUM_ARTIST"), u"ALBUMARTIST"_s);
    EXPECT_EQ(canonicalTagKey(u"Key"), u"INITIALKEY"_s);
    EXPECT_EQ(canonicalTagKey(u"ORGANIZATION"), u"LABEL"_s);
    EXPECT_EQ(canonicalTagKey(u"PUBLISHER"), u"LABEL"_s);
    EXPECT_EQ(canonicalTagKey(u"DESCRIPTION"), u"COMMENT"_s);
    EXPECT_EQ(canonicalTagKey(u"UNSYNCEDLYRICS"), u"LYRICS"_s);
    EXPECT_EQ(canonicalTagKey(u"ORIGINALYEAR"), u"ORIGINALDATE"_s);
    EXPECT_EQ(canonicalTagKey(u"CATALOGID"), u"CATALOGNUMBER"_s);
    EXPECT_EQ(canonicalTagKey(u" title "), u"TITLE"_s);
    EXPECT_EQ(canonicalTagKey(u"MYCUSTOM"), u"MYCUSTOM"_s);
}

TEST(TagPolicy, DenyList)
{
    const TagPolicyOptions transcode; // defaults: transcoding, strip DJ data
    TagPolicyOptions keep;
    keep.transcoding = false;
    keep.stripDjAppData = false;
    for (const QStringView key :
         {u"ENCODEDBY", u"ENCODING", u"encoder", u"LENGTH", u"COMMENT:ITUNNORM", u"COMMENT:iTunSMPB"}) {
        EXPECT_TRUE(isDeniedTagKey(key, transcode)) << key.toString().toStdString();
        EXPECT_TRUE(isDeniedTagKey(key, keep)) << key.toString().toStdString();
    }
    for (const QStringView key :
         {u"REPLAYGAIN_TRACK_GAIN", u"replaygain_album_peak", u"MP3GAIN_MINMAX", u"R128_TRACK_GAIN"}) {
        EXPECT_TRUE(isDeniedTagKey(key, transcode));
        EXPECT_FALSE(isDeniedTagKey(key, keep)); // ReplayGain is kept when the audio is (D-7)
    }
    for (const QStringView key : {u"SERATO_MARKERS_V2", u"----:COM.SERATO.DJ:MARKERS", u"TRAKTOR4"}) {
        EXPECT_TRUE(isDeniedTagKey(key, transcode));
        EXPECT_FALSE(isDeniedTagKey(key, keep));
    }
    TagPolicyOptions stripOnly = keep;
    stripOnly.stripDjAppData = true; // D-11 setting for files kept as-is
    EXPECT_TRUE(isDeniedTagKey(u"SERATO_ANALYSIS", stripOnly));
    EXPECT_FALSE(isDeniedTagKey(u"REPLAYGAIN_TRACK_GAIN", stripOnly));
    for (const QStringView key : {u"TITLE", u"COMMENT", u"COMMENT:SOMETHING", u"INITIALKEY", u"LABEL", u"MYCUSTOM"}) {
        EXPECT_FALSE(isDeniedTagKey(key, transcode)) << key.toString().toStdString();
    }
}

TEST(TagPolicy, AppliedModel)
{
    MetadataModel source;
    source.set(keys::Title, u"Song"_s, Provenance::Embedded);
    source.set(u"REPLAYGAIN_TRACK_GAIN", u"-6.5 dB"_s, Provenance::Embedded);
    source.set(keys::Encoding, u"Some Encoder 1.0"_s, Provenance::Embedded);
    source.set(u"SERATO_MARKERS_V2", u"base64"_s, Provenance::Embedded);
    Picture cover;
    cover.data = "cover";
    source.setFrontCover(cover);
    TagPolicyOptions options;
    options.encodedBy = u"Selecta 0.1.0"_s;
    options.encoderSettings = u"LAME3.100 -b 320"_s;
    const MetadataModel out = applyTagPolicy(source, options);
    EXPECT_EQ(out.value(keys::Title), u"Song"_s);
    EXPECT_FALSE(out.contains(u"REPLAYGAIN_TRACK_GAIN"));
    EXPECT_FALSE(out.contains(u"SERATO_MARKERS_V2"));
    EXPECT_EQ(out.value(keys::Encoding), u"LAME3.100 -b 320"_s); // never the source's value
    EXPECT_EQ(out.value(keys::EncodedBy), u"Selecta 0.1.0"_s);
    EXPECT_EQ(out.provenance(keys::EncodedBy), Provenance::Selecta);
    EXPECT_EQ(out.pictures(), source.pictures());
}
