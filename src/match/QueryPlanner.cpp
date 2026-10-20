// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/QueryPlanner.h"

#include "match/Lexicon.h"
#include "match/Normalizer.h"
#include "match/VersionClassifier.h"
#include "match/detail/Text.h"

#include <QChar>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace lexicon = detail::lexicon;

namespace {

enum class WordSource : quint8 {
    Artist,
    Title,
    Remixer,
};

// One candidate query word and what it is made of.
struct Word
{
    QString text; // case-folded, spelled as the release
    WordSource source = WordSource::Title;
    bool risky = false;  // a diacritic ("röyksopp"), or a piece of a word split at an apostrophe or a symbol
                         // ("don" of "don't", "ke" of "ke$ha"): files spell these several ways
    qsizetype order = 0; // position in the release text
};

// Characters as a reader counts them: code points without combining marks.
qsizetype visibleLength(QStringView word)
{
    qsizetype length = 0;
    for (qsizetype i = 0; i < word.size();) {
        const auto [codePoint, width] = detail::codePointAt(word, i);
        if (QChar::category(codePoint) != QChar::Mark_NonSpacing) {
            ++length;
        }
        i += width;
    }
    return length;
}

bool hasDiacritic(QStringView text)
{
    return std::ranges::any_of(text, [](QChar c) { return c.unicode() >= 0x80 && c.isLetter(); });
}

bool hasApostropheOrSymbol(QStringView chunk)
{
    return std::ranges::any_of(
        chunk, [](QChar c) { return lexicon::isApostrophe(c.unicode()) || c == u'$' || c == u'@' || c == u'!'; });
}

// The words of `text` as a responder indexes them, with risky spellings marked.
QList<Word> wordsOf(QStringView text, WordSource source, qsizetype &order)
{
    QList<Word> words;
    const QList<QStringView> chunks = detail::words(text);
    for (const QStringView chunk : chunks) {
        const QStringList pieces = QueryPlanner::responderWords(chunk);
        const bool split = pieces.size() > 1 && hasApostropheOrSymbol(chunk);
        for (const QString &piece : pieces) {
            words.append({piece, source, split || hasDiacritic(piece), order++});
        }
    }
    return words;
}

// "röyksopp" → "royksopp": the folded spelling of a word (spelling variant, §17.7 ladder step 1).
QString folded(const QString &word)
{
    QString out;
    const QString decomposed = word.normalized(QString::NormalizationForm_KD);
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        if (const QStringView latin = lexicon::transliteration(c.unicode()); !latin.isEmpty()) {
            out.append(latin);
        } else {
            out.append(c);
        }
    }
    return out.normalized(QString::NormalizationForm_C);
}

bool selective(const QList<Word> &words)
{
    const auto long4 = std::ranges::count_if(words, [](const Word &w) { return visibleLength(w.text) >= 4; });
    const bool long6 = std::ranges::any_of(words, [](const Word &w) { return visibleLength(w.text) >= 6; });
    return long4 >= 2 || long6;
}

struct Selection
{
    QList<Word> words;
    QStringList notes;
};

// The default word choice (note §17.7 design step 2).
Selection chooseWords(const ReleaseReference &release)
{
    Selection selection;
    qsizetype order = 0;
    const TitleParts parts = VersionClassifier::decompose(release.title);
    QList<Word> words;
    if (!release.artists.isEmpty()) {
        words += wordsOf(release.artists.front(), WordSource::Artist, order);
    }
    words += wordsOf(parts.core, WordSource::Title, order);
    if (parts.version.versionClass == VersionClass::Remix || parts.version.versionClass == VersionClass::Vip) {
        words += wordsOf(parts.version.byDisplay, WordSource::Remixer, order);
    }
    // Fragments of a split word, words under 3 characters, stop and version words.
    QList<Word> kept;
    QList<Word> titleStops;
    for (const Word &word : std::as_const(words)) {
        if (visibleLength(word.text) < kMinQueryWordLength) {
            selection.notes.append(u"'%1' left out: shorter than 3 characters"_s.arg(word.text));
        } else if (lexicon::isQueryStopWord(word.text)) {
            if (word.source == WordSource::Title) {
                titleStops.append(word);
            }
            selection.notes.append(u"'%1' left out: a stop or version word"_s.arg(word.text));
        } else {
            kept.append(word);
        }
    }
    const auto titleWords = [](const QList<Word> &list) {
        return std::ranges::count_if(list, [](const Word &w) { return w.source == WordSource::Title; });
    };
    if (titleWords(kept) == 0 && !titleStops.isEmpty()) {
        kept += titleStops; // a title made only of stop words ("Mix") keeps them
    }
    // Risky spellings only when the rest isn't selective enough (tagged files usually keep diacritics).
    QList<Word> plain;
    for (const Word &word : std::as_const(kept)) {
        if (!word.risky) {
            plain.append(word);
        }
    }
    if (plain.size() < kept.size() && selective(plain)) {
        for (const Word &word : std::as_const(kept)) {
            if (word.risky) {
                selection.notes.append(u"'%1' left out: spelled several ways, and the other words are selective "
                                       u"enough"_s.arg(word.text));
            }
        }
        kept = plain;
    }
    // At most kMaxQueryWords: drop the shortest (artist words before title words at equal length).
    while (kept.size() > kMaxQueryWords) {
        const auto shortest = std::ranges::min_element(kept, [](const Word &a, const Word &b) {
            return std::make_tuple(visibleLength(a.text), a.source == WordSource::Title, -a.order) <
                   std::make_tuple(visibleLength(b.text), b.source == WordSource::Title, -b.order);
        });
        selection.notes.append(u"'%1' left out: at most %2 words"_s.arg(shortest->text).arg(kMaxQueryWords));
        kept.erase(shortest);
    }
    std::ranges::sort(kept, {}, &Word::order);
    selection.words = std::move(kept);
    return selection;
}

QStringList textsOf(const QList<Word> &words)
{
    QStringList texts;
    for (const Word &word : words) {
        texts.append(word.text);
    }
    return texts;
}

} // namespace

QueryPlanner::QueryPlanner(const QStringList &excludedPhrases)
{
    for (const QString &phrase : excludedPhrases) {
        const QString key = phrase.trimmed().toCaseFolded();
        if (!key.isEmpty()) {
            m_excludedPhrases.append(key);
        }
    }
}

QStringList QueryPlanner::responderWords(QStringView text)
{
    const QString normalised = text.toString().normalized(QString::NormalizationForm_C);
    QStringList words;
    QString word;
    for (qsizetype i = 0; i < normalised.size();) {
        const auto [codePoint, width] = detail::codePointAt(normalised, i);
        if (lexicon::isResponderSeparator(codePoint)) {
            if (!word.isEmpty()) {
                words.append(word.toCaseFolded());
                word.clear();
            }
        } else {
            word.append(QStringView(normalised).sliced(i, width));
        }
        i += width;
    }
    if (!word.isEmpty()) {
        words.append(word.toCaseFolded());
    }
    return words;
}

QueryPlan QueryPlanner::plan(const ReleaseReference &release, QueryStep step, QStringView typed) const
{
    QueryPlan plan;
    plan.step = step;
    const Selection selection = chooseWords(release);
    QList<Word> words = selection.words;
    plan.notes = selection.notes;
    switch (step) {
    case QueryStep::Default:
        break;
    case QueryStep::SpellingVariant: {
        // Diacritics folded, and a multi-word artist joined into one word: a different spelling, never a wildcard.
        QList<Word> artist;
        QList<Word> rest;
        for (Word &word : words) {
            word.text = folded(word.text);
            (word.source == WordSource::Artist ? artist : rest).append(word);
        }
        if (artist.size() >= 2) {
            Word joined = artist.front();
            joined.text = textsOf(artist).join(QString());
            artist = {joined};
            plan.notes.append(u"artist joined into '%1'"_s.arg(joined.text));
        }
        words = artist + rest;
        words.removeIf([](const Word &w) { return visibleLength(w.text) < kMinQueryWordLength; });
        break;
    }
    case QueryStep::FewerWords: {
        // The title's words without symbol-split pieces; if that changes nothing, drop the longest word.
        QList<Word> title;
        for (const Word &word : std::as_const(words)) {
            if (word.source == WordSource::Title) {
                title.append(word);
            }
        }
        if (!title.isEmpty() && title.size() < words.size()) {
            words = title;
        } else if (words.size() >= 2) {
            words.erase(std::ranges::max_element(words, {}, [](const Word &w) { return visibleLength(w.text); }));
        }
        break;
    }
    case QueryStep::ArtistOnly: {
        QList<Word> artist;
        for (const Word &word : std::as_const(words)) {
            if (word.source == WordSource::Artist) {
                artist.append(word);
            }
        }
        words = artist;
        break;
    }
    }
    plan.words = textsOf(words);
    plan.text = plan.words.join(u' ');
    if (plan.text.size() > kMaxQueryLength) {
        plan.text.truncate(kMaxQueryLength);
    }
    if (!typed.isEmpty()) {
        QStringList typedWords;
        const QStringList typedSplit = responderWords(typed);
        for (const QString &word : typedSplit) {
            if (visibleLength(word) >= kMinQueryWordLength && !lexicon::isQueryStopWord(word)) {
                typedWords.append(folded(word));
            }
        }
        QStringList planned;
        for (const QString &word : std::as_const(plan.words)) {
            planned.append(folded(word));
        }
        typedWords.sort();
        planned.sort();
        plan.respelled = typedWords != planned;
    }
    if (plan.words.isEmpty()) {
        plan.status = QueryStatus::NoWords;
        return plan;
    }
    for (const QString &phrase : m_excludedPhrases) {
        if (plan.text.contains(phrase)) {
            plan.status = QueryStatus::Blocked;
            plan.blockedPhrase = phrase;
            plan.notes.append(u"the server excludes '%1': compliant peers would return nothing"_s.arg(phrase));
            return plan;
        }
    }
    plan.status = QueryStatus::Ready;
    return plan;
}

QList<QueryPlan> QueryPlanner::widerLadder(const ReleaseReference &release) const
{
    QList<QueryPlan> ladder;
    QStringList sent{plan(release, QueryStep::Default).text};
    for (const QueryStep step : {QueryStep::SpellingVariant, QueryStep::FewerWords, QueryStep::ArtistOnly}) {
        QueryPlan wider = plan(release, step);
        if (wider.status == QueryStatus::Ready && sent.contains(wider.text)) {
            wider.status = QueryStatus::Unchanged;
        } else if (wider.status == QueryStatus::Ready) {
            sent.append(wider.text);
        }
        ladder.append(std::move(wider));
    }
    return ladder;
}

} // namespace selecta::match
