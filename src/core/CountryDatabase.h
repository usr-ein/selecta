// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHostAddress>
#include <QString>
#include <QStringView>

#include <memory>
#include <optional>

struct MMDB_s;

namespace selecta::core {

// Where peers are, by country, for the small flag next to their name in search results (P-002): a MaxMind DB file of
// countries (DB-IP's "IP to Country Lite", CC BY 4.0) read with libmaxminddb. Offline: a lookup reads the mapped file
// and nothing else. Only addresses Selecta already has from its peer connections are looked up.
//
// The file is untrusted (it was downloaded): open() refuses a file larger than the cap, one libmaxminddb can't read,
// one that isn't a country database and one whose sample lookup fails; every lookup goes through libmaxminddb's
// bounds checks and keeps only a code QLocale knows as a territory.
//
// Threading: immutable once open; countryOf() is safe from any thread. Ownership: keeps the file mapped until
// destroyed; share it as a std::shared_ptr<const CountryDatabase>.
class CountryDatabase
{
public:
    static constexpr qint64 kMaxBytes = qint64{128} * 1024 * 1024; // DB-IP's country file is about 20 MB

    // The database at `path`, or null with `error` set (the file is missing, too large, unreadable or not a country
    // database).
    static std::shared_ptr<const CountryDatabase> open(const QString &path, QString *error = nullptr,
                                                       qint64 maxBytes = kMaxBytes);

    ~CountryDatabase();
    CountryDatabase(const CountryDatabase &) = delete;
    CountryDatabase &operator=(const CountryDatabase &) = delete;
    CountryDatabase(CountryDatabase &&) = delete;
    CountryDatabase &operator=(CountryDatabase &&) = delete;

    // The ISO 3166-1 alpha-2 code of the address's country ("FR"), upper case; empty when the address isn't a global
    // unicast one, isn't in the database or maps to no real territory.
    QString countryOf(const QHostAddress &address) const;

private:
    CountryDatabase();
    // The code (empty when none), or nullopt when libmaxminddb found the file broken on the way.
    std::optional<QString> find(const QHostAddress &address) const;

    std::unique_ptr<MMDB_s> m_db;
    bool m_open = false;
};

// Whether `code` is an ISO 3166-1 alpha-2 code of a territory QLocale knows ("FR"; not "ZZ", "EU" or "fr"). Pure.
bool isCountryCode(QStringView code);

} // namespace selecta::core
