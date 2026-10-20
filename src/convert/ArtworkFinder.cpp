// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/ArtworkFinder.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>
#include <tuple>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr std::array<QStringView, 8> kImageExtensions = {u"jpg", u"jpeg", u"png", u"webp",
                                                         u"gif", u"bmp",  u"tif", u"tiff"};
constexpr std::array<QStringView, 3> kExactNames = {u"cover", u"folder", u"front"};
constexpr std::array<QStringView, 4> kCoverWords = {u"front", u"cover", u"folder", u"albumart"};
constexpr std::array<QStringView, 8> kExcludedWords = {u"back",   u"cd",   u"disc",    u"inlay",
                                                       u"inside", u"tray", u"booklet", u"label"};
constexpr std::array<QStringView, 3> kArtFolders = {u"scans", u"artwork", u"covers"};
constexpr double kSquareLow = 0.8;
constexpr double kSquareHigh = 1.25;

template <std::size_t N>
int positionIn(const std::array<QStringView, N> &list, QStringView word)
{
    const auto *const found = std::ranges::find(list, word);
    return found == list.end() ? -1 : static_cast<int>(found - list.begin());
}

struct Candidate
{
    int index = -1;
    int tier = 0; // 0: the album folder; 1: a Scans/, Artwork/ or Covers/ subfolder
    QString path;
    QString stem;
    QStringList words;
    QSize size;
};

std::optional<Candidate> parse(const CoverCandidate &input, int index)
{
    QString path = input.relativePath;
    path.replace(u'\\', u'/');
    QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.contains(u".."_s) || path.startsWith(u'/')) {
        return std::nullopt; // relative to the album folder, never above it
    }
    if (parts.first() == u"."_s) {
        parts.removeFirst();
    }
    Candidate candidate;
    candidate.index = index;
    candidate.path = parts.join(u'/');
    candidate.size = input.size;
    if (parts.size() == 2 && positionIn(kArtFolders, parts.first().toLower()) >= 0) {
        candidate.tier = 1;
    } else if (parts.size() != 1) {
        return std::nullopt;
    }
    const QString file = parts.last();
    const qsizetype dot = file.lastIndexOf(u'.');
    if (dot <= 0 || positionIn(kImageExtensions, file.sliced(dot + 1).toLower()) < 0) {
        return std::nullopt;
    }
    candidate.stem = file.left(dot).toLower();
    static const QRegularExpression separators(uR"([^\p{L}\p{N}]+)"_s);
    candidate.words = candidate.stem.split(separators, Qt::SkipEmptyParts);
    return candidate;
}

bool hasExcludedWord(const Candidate &candidate)
{
    return std::ranges::any_of(candidate.words,
                               [](const QString &word) { return positionIn(kExcludedWords, word) >= 0; });
}

// The best cover word in the name (its position in kCoverWords), or -1.
int coverWord(const Candidate &candidate)
{
    int best = -1;
    for (const QString &word : candidate.words) {
        const int position = positionIn(kCoverWords, word);
        if (position >= 0 && (best < 0 || position < best)) {
            best = position;
        }
    }
    return best;
}

bool roughlySquare(QSize size)
{
    if (!size.isValid() || size.isEmpty()) {
        return false;
    }
    const double ratio = static_cast<double>(size.width()) / size.height();
    return ratio >= kSquareLow && ratio <= kSquareHigh;
}

} // namespace

std::optional<CoverChoice> chooseFolderCover(const QList<CoverCandidate> &candidates)
{
    QList<Candidate> images;
    for (int i = 0; i < candidates.size(); ++i) {
        if (std::optional<Candidate> parsed = parse(candidates[i], i)) {
            images.append(std::move(*parsed));
        }
    }
    // By name: exact names (rank 0), then names with a cover word and no excluded word (rank 1); the album folder
    // before its subfolders, then the order of the name lists, then the shorter and alphabetically first path.
    const Candidate *best = nullptr;
    std::tuple<int, int, int, qsizetype, QString> bestKey;
    for (const Candidate &image : images) {
        const int exact = positionIn(kExactNames, image.stem);
        const int word = hasExcludedWord(image) ? -1 : coverWord(image);
        if (exact < 0 && word < 0) {
            continue;
        }
        auto key = exact >= 0 ? std::make_tuple(0, image.tier, exact, image.path.size(), image.path)
                              : std::make_tuple(1, image.tier, word, image.path.size(), image.path);
        if (best == nullptr || key < bestKey) {
            best = &image;
            bestKey = std::move(key);
        }
    }
    if (best != nullptr) {
        return CoverChoice{.index = best->index, .guessed = false};
    }
    // By elimination, shown for confirmation: the only image that isn't obviously something else, or the largest
    // roughly square one.
    QList<const Candidate *> eligible;
    for (const Candidate &image : images) {
        if (!hasExcludedWord(image)) {
            eligible.append(&image);
        }
    }
    if (eligible.size() == 1) {
        return CoverChoice{.index = eligible.first()->index, .guessed = true};
    }
    const Candidate *largest = nullptr;
    for (const Candidate *image : std::as_const(eligible)) {
        if (!roughlySquare(image->size)) {
            continue;
        }
        const qint64 area = qint64{image->size.width()} * image->size.height();
        const qint64 bestArea = largest == nullptr ? -1 : qint64{largest->size.width()} * largest->size.height();
        if (largest == nullptr || area > bestArea ||
            (area == bestArea && std::tie(image->tier, image->path) < std::tie(largest->tier, largest->path))) {
            largest = image;
        }
    }
    if (largest != nullptr) {
        return CoverChoice{.index = largest->index, .guessed = true};
    }
    return std::nullopt;
}

} // namespace selecta::convert
