// SPDX-License-Identifier: GPL-3.0-or-later
// The word lists of the match library: its one compiled-in data file (contract X1). Lists are sorted at compile
// time and checked for duplicates; lookups are binary searches. Entries are normalised text (Normalizer): lower case,
// no diacritics, words separated by single spaces. Changing an entry that affects a key requires bumping
// kNormVersion (Normalizer.h).
#include "match/Lexicon.h"

#include <QChar>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace selecta::match::detail::lexicon {

namespace {

using Word = std::u16string_view;

template <std::size_t N>
consteval std::array<Word, N> sortedWords(std::array<Word, N> words)
{
    std::ranges::sort(words);
    return words;
}

template <std::size_t N>
consteval bool strictlySorted(const std::array<Word, N> &words)
{
    return std::ranges::adjacent_find(words, std::ranges::greater_equal{}) == words.end();
}

Word toWord(QStringView text)
{
    return {text.utf16(), static_cast<std::size_t>(text.size())};
}

template <std::size_t N>
bool containsWord(const std::array<Word, N> &words, QStringView text)
{
    return std::ranges::binary_search(words, toWord(text));
}

template <typename Value, std::size_t N>
consteval std::array<std::pair<Word, Value>, N> sortedEntries(std::array<std::pair<Word, Value>, N> entries)
{
    std::ranges::sort(entries, {}, &std::pair<Word, Value>::first);
    return entries;
}

template <typename Value, std::size_t N>
consteval bool strictlySortedEntries(const std::array<std::pair<Word, Value>, N> &entries)
{
    return std::ranges::adjacent_find(entries, std::ranges::greater_equal{}, &std::pair<Word, Value>::first) ==
           entries.end();
}

template <typename Value, std::size_t N>
const Value *findEntry(const std::array<std::pair<Word, Value>, N> &entries, QStringView text)
{
    const Word key = toWord(text);
    const auto *it = std::ranges::lower_bound(entries, key, {}, &std::pair<Word, Value>::first);
    if (it == entries.end() || it->first != key) {
        return nullptr;
    }
    return &it->second;
}

// --- Normalisation ---------------------------------------------------------------------------------------------------

constexpr auto kLightWords = sortedWords(std::to_array<Word>({
    u"a",
    u"an",
    u"and",
    u"de",
    u"feat",
    u"featuring",
    u"ft",
    u"of",
    u"the",
    u"vs",
    u"with",
    u"x",
}));
static_assert(strictlySorted(kLightWords));

constexpr auto kNumberWords = sortedEntries(std::to_array<std::pair<Word, int>>({
    {u"one", 1},      {u"two", 2},        {u"three", 3},     {u"four", 4},      {u"five", 5},
    {u"six", 6},      {u"seven", 7},      {u"eight", 8},     {u"nine", 9},      {u"ten", 10},
    {u"eleven", 11},  {u"twelve", 12},    {u"thirteen", 13}, {u"fourteen", 14}, {u"fifteen", 15},
    {u"sixteen", 16}, {u"seventeen", 17}, {u"eighteen", 18}, {u"nineteen", 19}, {u"twenty", 20},
}));
static_assert(strictlySortedEntries(kNumberWords));

constexpr auto kCanonicalTokens = sortedEntries(std::to_array<std::pair<Word, Word>>({
    {u"pt", u"part"},
    {u"vol", u"volume"},
}));
static_assert(strictlySortedEntries(kCanonicalTokens));

// --- Versions --------------------------------------------------------------------------------------------------------

constexpr auto kVersionKeywords = sortedEntries(std::to_array<std::pair<Word, VersionWord>>({
    {u"bootleg", VersionWord::RemixKeyword},
    {u"dub", VersionWord::DubKeyword},
    {u"edit", VersionWord::EditKeyword},
    {u"flip", VersionWord::RemixKeyword},
    {u"mix", VersionWord::MixKeyword},
    {u"rebuild", VersionWord::RemixKeyword},
    {u"reconstruction", VersionWord::RemixKeyword},
    {u"reedit", VersionWord::EditKeyword},
    {u"refix", VersionWord::RemixKeyword},
    {u"remake", VersionWord::RemixKeyword},
    {u"remix", VersionWord::RemixKeyword},
    {u"remodel", VersionWord::RemixKeyword},
    {u"rerub", VersionWord::RemixKeyword},
    {u"rework", VersionWord::RemixKeyword},
    {u"rmx", VersionWord::RemixKeyword},
    {u"version", VersionWord::VersionKeyword},
    {u"vip", VersionWord::VipKeyword},
}));
static_assert(strictlySortedEntries(kVersionKeywords));

// "12" and "7" stand for 12" and 7" (the quote is punctuation): class words only before mix/version/edit/inch.
constexpr auto kClassWords = sortedEntries(std::to_array<std::pair<Word, ClassWord>>({
    {u"12", {VersionClass::Extended, VersionModifier::Extended, true}},
    {u"12inch", {VersionClass::Extended, VersionModifier::Extended, false}},
    {u"7", {VersionClass::Radio, VersionModifier::Radio, true}},
    {u"7inch", {VersionClass::Radio, VersionModifier::Radio, false}},
    {u"acapela", {VersionClass::Acapella, VersionModifier::Acapella, false}},
    {u"acapella", {VersionClass::Acapella, VersionModifier::Acapella, false}},
    {u"acappella", {VersionClass::Acapella, VersionModifier::Acapella, false}},
    {u"acoustic", {VersionClass::Acoustic, VersionModifier::Acoustic, false}},
    {u"album", {VersionClass::Original, VersionModifier::None, true}},
    {u"club", {VersionClass::Extended, VersionModifier::Extended, false}},
    {u"demo", {VersionClass::Demo, VersionModifier::Demo, false}},
    {u"extended", {VersionClass::Extended, VersionModifier::Extended, false}},
    {u"fulllength", {VersionClass::Extended, VersionModifier::Extended, false}},
    {u"inst", {VersionClass::Instrumental, VersionModifier::Instrumental, false}},
    {u"instr", {VersionClass::Instrumental, VersionModifier::Instrumental, false}},
    {u"instrumental", {VersionClass::Instrumental, VersionModifier::Instrumental, false}},
    {u"live", {VersionClass::Live, VersionModifier::Live, false}},
    {u"long", {VersionClass::Extended, VersionModifier::Extended, true}},
    {u"main", {VersionClass::Original, VersionModifier::None, true}},
    {u"maxi", {VersionClass::Extended, VersionModifier::Extended, true}},
    {u"original", {VersionClass::Original, VersionModifier::None, false}},
    {u"radio", {VersionClass::Radio, VersionModifier::Radio, false}},
    {u"short", {VersionClass::Radio, VersionModifier::Radio, true}},
    {u"single", {VersionClass::Radio, VersionModifier::Radio, true}},
    {u"unplugged", {VersionClass::Acoustic, VersionModifier::Acoustic, false}},
    {u"vocal", {VersionClass::Vocal, VersionModifier::Vocal, false}},
}));
static_assert(strictlySortedEntries(kClassWords));

constexpr auto kRemasterWords = sortedWords(std::to_array<Word>({
    u"digital",
    u"digitally",
    u"remaster",
    u"remastered",
    u"remasters",
    u"remastering",
}));
static_assert(strictlySorted(kRemasterWords));

constexpr auto kFeatWords = sortedWords(std::to_array<Word>({u"feat", u"featuring", u"ft"}));
static_assert(strictlySorted(kFeatWords));

constexpr auto kVariantWords = sortedWords(std::to_array<Word>({
    u"alt",     u"alternate",  u"alternative", u"censored",   u"chinese",    u"clean",   u"cover",
    u"dirty",   u"dutch",      u"early",       u"english",    u"explicit",   u"french",  u"full",
    u"german",  u"italian",    u"japanese",    u"karaoke",    u"korean",     u"mono",    u"orchestral",
    u"piano",   u"portuguese", u"rehearsal",   u"rough",      u"russian",    u"spanish", u"stereo",
    u"strings", u"swedish",    u"tv",          u"uncensored", u"unreleased",
}));
static_assert(strictlySorted(kVariantWords));

constexpr auto kWebJunkPhrases = sortedWords(std::to_array<Word>({
    u"1080p",        u"4k",         u"720p",          u"audio",       u"audio only", u"bonus",
    u"bonus track",  u"clip",       u"clip officiel", u"free",        u"free dl",    u"free download",
    u"freedownload", u"full song",  u"full track",    u"hd",          u"hd 1080p",   u"high quality",
    u"hq",           u"hq audio",   u"lyric",         u"lyric video", u"lyrics",     u"lyrics video",
    u"music video",  u"official",   u"out now",       u"premiere",    u"video",      u"video clip",
    u"videoclip",    u"visualiser", u"visualizer",    u"with lyrics",
}));
static_assert(strictlySorted(kWebJunkPhrases));

constexpr auto kWebJunkPrefixes = std::to_array<Word>({
    u"available on ",
    u"buy on ",
    u"download on ",
    u"free download ",
    u"official ",
    u"out now on ",
    u"prod ",
    u"produced by ",
});

// --- Paths -----------------------------------------------------------------------------------------------------------

constexpr auto kGenericFolders = sortedWords(std::to_array<Word>({
    u"0 9",
    u"00s",
    u"10s",
    u"128",
    u"16bit",
    u"192",
    u"1990s",
    u"2000s",
    u"2010s",
    u"2020s",
    u"20s",
    u"24 bit",
    u"24bit",
    u"256",
    u"320",
    u"60s",
    u"70s",
    u"80s",
    u"90s",
    u"a z",
    u"aac",
    u"acid house",
    u"afro",
    u"afro house",
    u"afrobeat",
    u"aiff",
    u"album",
    u"albums",
    u"alternative",
    u"amapiano",
    u"ambient",
    u"audio",
    u"baile funk",
    u"balearic",
    u"bass",
    u"bass music",
    u"blues",
    u"breakbeat",
    u"breaks",
    u"cd",
    u"cd flac",
    u"cds",
    u"chill",
    u"chillout",
    u"classical",
    u"collection",
    u"compilation",
    u"compilations",
    u"complete",
    u"completed",
    u"country",
    u"dance",
    u"dancehall",
    u"data",
    u"deep house",
    u"desktop",
    u"disco",
    u"dj",
    u"dj music",
    u"dj tracks",
    u"djs",
    u"dnb",
    u"documents",
    u"download",
    u"downloads",
    u"downtempo",
    u"drum and bass",
    u"drum n bass",
    u"dub",
    u"dubstep",
    u"edm",
    u"electro",
    u"electronic",
    u"electronica",
    u"experimental",
    u"favorites",
    u"favourites",
    u"files",
    u"flac",
    u"flac 16",
    u"flac 24",
    u"folk",
    u"footwork",
    u"funk",
    u"gabber",
    u"garage",
    u"goa",
    u"gqom",
    u"grime",
    u"hardcore",
    u"hardstyle",
    u"hi res",
    u"hip hop",
    u"hiphop",
    u"hires",
    u"home",
    u"house",
    u"idm",
    u"incoming",
    u"indie",
    u"italo",
    u"italo disco",
    u"itunes",
    u"itunes media",
    u"itunes music",
    u"j pop",
    u"jazz",
    u"juke",
    u"jungle",
    u"k pop",
    u"latin",
    u"leftfield",
    u"library",
    u"lossless",
    u"lounge",
    u"m4a",
    u"media",
    u"metal",
    u"minimal",
    u"misc",
    u"miscellaneous",
    u"mixes",
    u"mp3",
    u"mp3 320",
    u"mp3s",
    u"music",
    u"my music",
    u"new",
    u"new music",
    u"new wave",
    u"nicotine",
    u"nu disco",
    u"ogg",
    u"opus",
    u"ost",
    u"other",
    u"others",
    u"playlist",
    u"playlists",
    u"pop",
    u"progressive",
    u"progressive house",
    u"promo",
    u"promos",
    u"psy trance",
    u"psytrance",
    u"public",
    u"punk",
    u"r and b",
    u"rap",
    u"reggae",
    u"reggaeton",
    u"rips",
    u"rnb",
    u"rock",
    u"sets",
    u"share",
    u"shared",
    u"shares",
    u"single",
    u"singles",
    u"slsk",
    u"songs",
    u"soul",
    u"soulseek",
    u"soulseek downloads",
    u"soundtrack",
    u"soundtracks",
    u"synthwave",
    u"tech house",
    u"techno",
    u"temp",
    u"tmp",
    u"torrents",
    u"tracks",
    u"trance",
    u"trap",
    u"uk garage",
    u"ukg",
    u"unknown",
    u"unsorted",
    u"user",
    u"users",
    u"v0",
    u"v2",
    u"various",
    u"vinyl",
    u"vinyl rip",
    u"wav",
    u"web",
    u"web flac",
    u"world",
}));
static_assert(strictlySorted(kGenericFolders));

constexpr auto kVariousArtists = sortedWords(std::to_array<Word>({
    u"compilation",
    u"compilations",
    u"v a",
    u"va",
    u"various",
    u"various artist",
    u"various artists",
}));
static_assert(strictlySorted(kVariousArtists));

constexpr auto kQualityWords = sortedEntries(std::to_array<std::pair<Word, QualityWord>>({
    {u"1", QualityWord::Weak},         {u"128", QualityWord::Strong},     {u"128k", QualityWord::Strong},
    {u"128kbps", QualityWord::Strong}, {u"16", QualityWord::Weak},        {u"160", QualityWord::Strong},
    {u"1644", QualityWord::Strong},    {u"16bit", QualityWord::Strong},   {u"176", QualityWord::Weak},
    {u"192", QualityWord::Strong},     {u"192k", QualityWord::Strong},    {u"192kbps", QualityWord::Strong},
    {u"192khz", QualityWord::Strong},  {u"2", QualityWord::Weak},         {u"224", QualityWord::Strong},
    {u"24", QualityWord::Weak},        {u"24192", QualityWord::Strong},   {u"2444", QualityWord::Strong},
    {u"2448", QualityWord::Strong},    {u"2488", QualityWord::Strong},    {u"2496", QualityWord::Strong},
    {u"24bit", QualityWord::Strong},   {u"256", QualityWord::Strong},     {u"256k", QualityWord::Strong},
    {u"256kbps", QualityWord::Strong}, {u"32", QualityWord::Weak},        {u"320", QualityWord::Strong},
    {u"320k", QualityWord::Strong},    {u"320kbps", QualityWord::Strong}, {u"32bit", QualityWord::Strong},
    {u"44", QualityWord::Weak},        {u"44khz", QualityWord::Strong},   {u"48", QualityWord::Weak},
    {u"48khz", QualityWord::Strong},   {u"88", QualityWord::Weak},        {u"96", QualityWord::Weak},
    {u"96khz", QualityWord::Strong},   {u"aac", QualityWord::Strong},     {u"aiff", QualityWord::Strong},
    {u"alac", QualityWord::Strong},    {u"bit", QualityWord::Weak},       {u"cbr", QualityWord::Strong},
    {u"cd", QualityWord::Strong},      {u"cdr", QualityWord::Strong},     {u"dsd", QualityWord::Strong},
    {u"flac", QualityWord::Strong},    {u"hi", QualityWord::Weak},        {u"hires", QualityWord::Strong},
    {u"kbps", QualityWord::Weak},      {u"khz", QualityWord::Weak},       {u"lossless", QualityWord::Strong},
    {u"mp3", QualityWord::Strong},     {u"ogg", QualityWord::Strong},     {u"opus", QualityWord::Strong},
    {u"res", QualityWord::Weak},       {u"rip", QualityWord::Weak},       {u"sacd", QualityWord::Strong},
    {u"v0", QualityWord::Strong},      {u"v2", QualityWord::Strong},      {u"vbr", QualityWord::Strong},
    {u"vinyl", QualityWord::Strong},   {u"wav", QualityWord::Strong},     {u"web", QualityWord::Strong},
}));
static_assert(strictlySortedEntries(kQualityWords));

constexpr auto kSceneSources = sortedEntries(std::to_array<std::pair<Word, Word>>({
    {u"bd", u"BD"},
    {u"cable", u"CABLE"},
    {u"cd", u"CD"},
    {u"cdm", u"CDM"},
    {u"cdr", u"CDR"},
    {u"cds", u"CDS"},
    {u"dab", u"DAB"},
    {u"dvd", u"DVD"},
    {u"fm", u"FM"},
    {u"line", u"LINE"},
    {u"sat", u"SAT"},
    {u"sbd", u"SBD"},
    {u"stream", u"STREAM"},
    {u"tape", u"TAPE"},
    {u"vinyl", u"VINYL"},
    {u"vls", u"VLS"},
    {u"web", u"WEB"},
}));
static_assert(strictlySortedEntries(kSceneSources));

constexpr auto kEditionWords = sortedWords(std::to_array<Word>({
    u"10th",     u"20th",    u"25th",        u"2cd",      u"30th",    u"3cd",      u"40th",      u"4cd",
    u"50th",     u"album",   u"anniversary", u"bonus",    u"cd",      u"clean",    u"collector", u"collectors",
    u"deluxe",   u"digital", u"disc",        u"discs",    u"edition", u"editions", u"ep",        u"expanded",
    u"explicit", u"flac",    u"issue",       u"legacy",   u"limited", u"lp",       u"maxi",      u"mono",
    u"mp3",      u"of",      u"original",    u"platinum", u"re",      u"reissue",  u"remaster",  u"remastered",
    u"s",        u"single",  u"special",     u"stereo",   u"super",   u"the",      u"tour",      u"track",
    u"tracks",   u"version", u"vinyl",       u"web",
}));
static_assert(strictlySorted(kEditionWords));

constexpr auto kStrongEditionWords = sortedWords(std::to_array<Word>({
    u"anniversary",
    u"bonus",
    u"collectors",
    u"deluxe",
    u"edition",
    u"expanded",
    u"legacy",
    u"limited",
    u"platinum",
    u"reissue",
    u"remaster",
    u"remastered",
    u"special",
}));
static_assert(strictlySorted(kStrongEditionWords));

constexpr auto kReleaseTypeWords = sortedWords(std::to_array<Word>({u"album", u"ep", u"lp", u"maxi", u"single"}));
static_assert(strictlySorted(kReleaseTypeWords));

constexpr auto kWebsiteTlds = sortedWords(std::to_array<Word>({
    u"at",   u"au",    u"be",  u"bg", u"biz", u"br",     u"ca",  u"cc", u"ch", u"co",   u"com", u"cz",
    u"de",   u"dk",    u"es",  u"eu", u"fi",  u"fm",     u"fr",  u"gr", u"hu", u"info", u"io",  u"it",
    u"me",   u"music", u"net", u"nl", u"no",  u"online", u"org", u"pl", u"pt", u"ro",   u"ru",  u"se",
    u"site", u"sk",    u"su",  u"to", u"top", u"tv",     u"ua",  u"uk", u"us", u"ws",   u"xyz",
}));
static_assert(strictlySorted(kWebsiteTlds));

constexpr auto kRipWords = sortedWords(std::to_array<Word>({
    u"audiotrack",
    u"faixa",
    u"piste",
    u"pista",
    u"spur",
    u"titel",
    u"titre",
    u"traccia",
    u"track",
}));
static_assert(strictlySorted(kRipWords));

// --- File kinds ------------------------------------------------------------------------------------------------------

constexpr auto kAudioExtensions = sortedWords(std::to_array<Word>({
    u"aac",
    u"aif",
    u"aiff",
    u"alac",
    u"ape",
    u"flac",
    u"m4a",
    u"mp3",
    u"ogg",
    u"opus",
    u"wav",
    u"wma",
    u"wv",
}));
static_assert(strictlySorted(kAudioExtensions));

constexpr auto kCompanionExtensions = sortedWords(std::to_array<Word>({
    u"accurip", u"bmp", u"cue", u"ffp", u"gif", u"jpeg", u"jpg", u"log",  u"m3u", u"m3u8",
    u"md5",     u"nfo", u"pdf", u"png", u"sfv", u"st5",  u"tif", u"tiff", u"txt", u"webp",
}));
static_assert(strictlySorted(kCompanionExtensions));

// --- Queries ---------------------------------------------------------------------------------------------------------

constexpr auto kQueryStopWords = sortedWords(std::to_array<Word>({
    u"a",        u"acapella", u"acoustic",  u"an",      u"and",          u"club",  u"dub",      u"edit",
    u"extended", u"feat",     u"featuring", u"ft",      u"instrumental", u"mix",   u"original", u"radio",
    u"remix",    u"rmx",      u"the",       u"version", u"vip",          u"vocal", u"vs",
}));
static_assert(strictlySorted(kQueryStopWords));

} // namespace

// --- Normalisation ---------------------------------------------------------------------------------------------------

QStringView transliteration(char32_t codePoint)
{
    switch (codePoint) {
    case U'ø': // ø
        return u"o";
    case U'æ': // æ
        return u"ae";
    case U'œ': // œ
        return u"oe";
    case U'ß': // ß
        return u"ss";
    case U'ł': // ł
        return u"l";
    case U'đ': // đ
    case U'ð': // ð
        return u"d";
    case U'þ': // þ
        return u"th";
    case U'ı': // ı
        return u"i";
    case U'ħ': // ħ
        return u"h";
    case U'ŧ': // ŧ
        return u"t";
    case U'ŋ': // ŋ
        return u"n";
    case U'ƒ': // ƒ
        return u"f";
    case U'ĸ': // ĸ
        return u"k";
    case U'ƀ': // ƀ
        return u"b";
    case U'ǥ': // ǥ
        return u"g";
    default:
        return {};
    }
}

bool isApostrophe(char32_t codePoint)
{
    switch (codePoint) {
    case U'\'':
    case U'`':
    case U'´':  // ´
    case U'ʹ':  // ʹ
    case U'ʻ':  // ʻ
    case U'ʼ':  // ʼ
    case U'‘':  // ‘
    case U'’':  // ’
    case U'‛':  // ‛
    case U'′':  // ′
    case U'＇': // fullwidth '
        return true;
    default:
        return false;
    }
}

char16_t leetFold(char16_t c)
{
    switch (c) {
    case u'0':
        return u'o';
    case u'1':
    case u'!':
        return u'i';
    case u'3':
        return u'e';
    case u'4':
    case u'@':
        return u'a';
    case u'5':
    case u'$':
        return u's';
    case u'7':
        return u't';
    default:
        return 0;
    }
}

std::optional<int> numberWord(QStringView token)
{
    if (const int *value = findEntry(kNumberWords, token)) {
        return *value;
    }
    return std::nullopt;
}

bool isNumberingWord(QStringView token)
{
    return token == u"part" || token == u"volume" || token == u"chapter";
}

QStringView canonicalToken(QStringView token)
{
    if (const Word *canonical = findEntry(kCanonicalTokens, token)) {
        return {canonical->data(), static_cast<qsizetype>(canonical->size())};
    }
    return {};
}

bool isLightWord(QStringView token)
{
    return containsWord(kLightWords, token);
}

// --- Versions --------------------------------------------------------------------------------------------------------

VersionWord versionWord(QStringView token)
{
    if (const VersionWord *word = findEntry(kVersionKeywords, token)) {
        return *word;
    }
    if (findEntry(kClassWords, token) != nullptr) {
        return VersionWord::ClassWord;
    }
    return VersionWord::None;
}

std::optional<ClassWord> classWord(QStringView token)
{
    if (const ClassWord *word = findEntry(kClassWords, token)) {
        return *word;
    }
    return std::nullopt;
}

int classPrecedence(VersionClass versionClass)
{
    switch (versionClass) {
    case VersionClass::Vip:
        return 11;
    case VersionClass::Acapella:
        return 10;
    case VersionClass::Instrumental:
        return 9;
    case VersionClass::Dub:
        return 8;
    case VersionClass::Live:
        return 7;
    case VersionClass::Acoustic:
        return 6;
    case VersionClass::Demo:
        return 5;
    case VersionClass::Extended:
        return 4;
    case VersionClass::Radio:
        return 3;
    case VersionClass::Vocal:
        return 2;
    case VersionClass::Original:
        return 1;
    case VersionClass::Remix:
        return 12; // a named remix outranks every class word
    case VersionClass::Unspecified:
        return 0;
    }
    return 0;
}

VersionModifier modifierFor(VersionClass versionClass)
{
    switch (versionClass) {
    case VersionClass::Extended:
        return VersionModifier::Extended;
    case VersionClass::Radio:
        return VersionModifier::Radio;
    case VersionClass::Instrumental:
        return VersionModifier::Instrumental;
    case VersionClass::Vocal:
        return VersionModifier::Vocal;
    case VersionClass::Dub:
        return VersionModifier::Dub;
    case VersionClass::Acapella:
        return VersionModifier::Acapella;
    case VersionClass::Live:
        return VersionModifier::Live;
    case VersionClass::Acoustic:
        return VersionModifier::Acoustic;
    case VersionClass::Demo:
        return VersionModifier::Demo;
    case VersionClass::Vip:
        return VersionModifier::Vip;
    case VersionClass::Unspecified:
    case VersionClass::Original:
    case VersionClass::Remix:
        return VersionModifier::None;
    }
    return VersionModifier::None;
}

bool isRemasterWord(QStringView token)
{
    return containsWord(kRemasterWords, token);
}

bool isMixedWord(QStringView token)
{
    return token == u"mixed";
}

bool isFeatWord(QStringView token)
{
    return containsWord(kFeatWords, token);
}

bool isVariantWord(QStringView token)
{
    return containsWord(kVariantWords, token);
}

bool isWebJunkPhrase(QStringView normalisedPhrase)
{
    if (containsWord(kWebJunkPhrases, normalisedPhrase)) {
        return true;
    }
    const Word phrase = toWord(normalisedPhrase);
    return std::ranges::any_of(kWebJunkPrefixes, [&phrase](Word prefix) { return phrase.starts_with(prefix); });
}

// --- Paths -----------------------------------------------------------------------------------------------------------

bool isGenericFolder(QStringView normalisedName)
{
    if (normalisedName.isEmpty()) {
        return true;
    }
    // Letter buckets ("a", "b", …) and years ("1999", "2024").
    if (normalisedName.size() == 1 && normalisedName.front().isLetter()) {
        return true;
    }
    if (normalisedName.size() == 4 && std::ranges::all_of(normalisedName, [](QChar c) { return c.isDigit(); }) &&
        (normalisedName.startsWith(u"19") || normalisedName.startsWith(u"20"))) {
        return true;
    }
    return containsWord(kGenericFolders, normalisedName);
}

bool isVariousArtists(QStringView normalisedName)
{
    return containsWord(kVariousArtists, normalisedName);
}

QualityWord qualityWord(QStringView token)
{
    if (const QualityWord *word = findEntry(kQualityWords, token)) {
        return *word;
    }
    return QualityWord::None;
}

QStringView sceneSource(QStringView token)
{
    if (const Word *display = findEntry(kSceneSources, token)) {
        return {display->data(), static_cast<qsizetype>(display->size())};
    }
    return {};
}

bool isEditionWord(QStringView token)
{
    return containsWord(kEditionWords, token);
}

bool isStrongEditionWord(QStringView token)
{
    return containsWord(kStrongEditionWords, token);
}

bool isReleaseTypeWord(QStringView token)
{
    return containsWord(kReleaseTypeWords, token);
}

bool isWebsiteTld(QStringView token)
{
    return containsWord(kWebsiteTlds, token);
}

bool isRipWord(QStringView token)
{
    return containsWord(kRipWords, token);
}

// --- File kinds ------------------------------------------------------------------------------------------------------

bool isAudioExtension(QStringView lowerExtension)
{
    return containsWord(kAudioExtensions, lowerExtension);
}

bool isCompanionExtension(QStringView lowerExtension)
{
    return containsWord(kCompanionExtensions, lowerExtension);
}

// --- Queries ---------------------------------------------------------------------------------------------------------

bool isQueryStopWord(QStringView foldedWord)
{
    return containsWord(kQueryStopWords, foldedWord);
}

bool isResponderSeparator(char32_t codePoint)
{
    if (codePoint < 0x80) {
        // Nicotine+ PUNCTUATION (pynicotine/utils.py): every ASCII punctuation character, plus whitespace.
        return codePoint <= 0x20 || codePoint == 0x7F || (codePoint >= U'!' && codePoint <= U'/') ||
               (codePoint >= U':' && codePoint <= U'@') || (codePoint >= U'[' && codePoint <= U'`') ||
               (codePoint >= U'{' && codePoint <= U'~');
    }
    switch (QChar::category(codePoint)) {
    case QChar::Punctuation_Connector:
    case QChar::Punctuation_Dash:
    case QChar::Punctuation_Open:
    case QChar::Punctuation_Close:
    case QChar::Punctuation_InitialQuote:
    case QChar::Punctuation_FinalQuote:
    case QChar::Punctuation_Other:
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return true;
    default:
        return false;
    }
}

} // namespace selecta::match::detail::lexicon
