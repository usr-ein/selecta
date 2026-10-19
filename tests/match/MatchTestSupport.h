// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <ostream>
#include <string>

// Readable GoogleTest output for Qt strings (PrintTo is found by argument-dependent lookup in the global namespace).
inline void PrintTo(const QString &text, std::ostream *os)
{
    *os << '"' << text.toStdString() << '"';
}

inline void PrintTo(const QStringList &list, std::ostream *os)
{
    *os << '[';
    for (qsizetype i = 0; i < list.size(); ++i) {
        *os << (i == 0 ? "" : ", ") << '"' << list.at(i).toStdString() << '"';
    }
    *os << ']';
}

namespace selecta::match {

inline void PrintTo(VersionClass value, std::ostream *os)
{
    *os << versionClassName(value).toStdString();
}

inline void PrintTo(VersionCompatibility value, std::ostream *os)
{
    *os << compatibilityName(value).toStdString();
}

inline void PrintTo(Tier value, std::ostream *os)
{
    *os << tierName(value).toStdString();
}

inline void PrintTo(Relation value, std::ostream *os)
{
    *os << relationName(value).toStdString();
}

} // namespace selecta::match

namespace selecta::match::test {

// A GoogleTest-safe test name from free text: letters and digits kept, everything else becomes "_".
inline std::string testName(const std::string &text)
{
    std::string name;
    for (const char c : text) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        name.push_back(alnum ? c : '_');
    }
    return name.empty() ? std::string("empty") : name;
}

// A small deterministic generator for property tests (SplitMix64): the same cases on every run and platform.
class Random
{
public:
    explicit Random(std::uint64_t seed)
        : m_state(seed)
    {}

    std::uint64_t next()
    {
        m_state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    // Uniform in [0, bound).
    qsizetype below(qsizetype bound) { return static_cast<qsizetype>(next() % static_cast<std::uint64_t>(bound)); }

private:
    std::uint64_t m_state;
};

// Random text drawn from characters that exercise every normalisation rule.
inline QString randomText(Random &random, qsizetype maxLength)
{
    static const QString pool =
        QStringLiteral("abcdeOKUMAdrift XYZ 0123456789 &+$@!'’.,-_/()[]{}:;éèÉøØæßłđðþıİ́̈ﬁＡｂ²½—–«»"
                       "Ãƒ¶©€™ МоскваΑθήνα東京한국\U0001F600\t");
    const qsizetype length = random.below(maxLength + 1);
    QString text;
    for (qsizetype i = 0; i < length; ++i) {
        const QChar c = pool.at(random.below(pool.size()));
        text.append(c);
    }
    return text;
}

} // namespace selecta::match::test
