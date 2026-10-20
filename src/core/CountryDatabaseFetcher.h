// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/CountryDatabase.h"

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace selecta::core {

// Where the country database comes from (P-002). Empty: no flags, and nothing is ever downloaded (tests and isolated
// runs, which point `sources` at a local fake instead).
struct CountryDatabaseOptions
{
    QString path;          // the database on disk: <data>/GeoIP/dbip-country-lite.mmdb
    QList<QUrl> sources;   // tried in order, once each, while the file is missing: https, or http on loopback only
    int stallMs = 30'000;  // a download that makes no progress for this long fails
    int totalMs = 180'000; // the whole fetch, every source included
};

// DB-IP's free country database for `today`'s month and the month before (it is published early in the month):
// https://download.db-ip.com/free/dbip-country-lite-YYYY-MM.mmdb.gz. Pure.
QList<QUrl> dbIpCountryLiteSources(QDate today);

// Opens the country database, downloading it first when it isn't on disk yet: the first run's one download (P-002).
// Bounded: kMaxDownloadBytes compressed, CountryDatabase::kMaxBytes inflated, the options' stallMs without progress
// and totalMs overall. The file is untrusted: it is inflated on a worker, written next to its place and opened as a
// database (CountryDatabase validates it) before it takes that place. A file on disk that fails to open is removed and
// fetched again, once. Any failure means no flags for this run: one attempt per source, no retry loop; the next launch
// tries again only while the file is still missing.
//
// Threading: lives on the engine thread; the inflating and the validation run on its own worker. Ownership: the
// network manager outlives it. Failure: failed() once, with the reason, for the log.
class CountryDatabaseFetcher : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 kMaxDownloadBytes = qint64{32} * 1024 * 1024; // DB-IP's .mmdb.gz is about 8 MB

    CountryDatabaseFetcher(CountryDatabaseOptions options, QNetworkAccessManager &network, QObject *parent = nullptr);
    ~CountryDatabaseFetcher() override;
    CountryDatabaseFetcher(const CountryDatabaseFetcher &) = delete;
    CountryDatabaseFetcher &operator=(const CountryDatabaseFetcher &) = delete;
    CountryDatabaseFetcher(CountryDatabaseFetcher &&) = delete;
    CountryDatabaseFetcher &operator=(CountryDatabaseFetcher &&) = delete;

    // Opens the file, or fetches it. Once; later calls do nothing.
    void start();

    // Inflates a gzip file of at most `maxOutput` bytes; empty with `error` set when it is corrupt, truncated, followed
    // by other data or larger than the cap. Pure; any thread.
    static QByteArray gunzip(const QByteArray &compressed, qint64 maxOutput, QString *error = nullptr);

Q_SIGNALS:
    void ready(const std::shared_ptr<const selecta::core::CountryDatabase> &database);
    void failed(const QString &why);

private:
    void fetchNext();
    void downloaded(const QByteArray &compressed);
    void fail(const QString &why);

    CountryDatabaseOptions m_options;
    // cppcheck-suppress uninitMemberVarNoCtor ; bound by the constructor, which cppcheck misses here
    QNetworkAccessManager &m_network;
    bool m_started = false;
    bool m_finished = false;
    qsizetype m_nextSource = 0;
    QString m_lastError;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QThreadPool m_worker;
};

} // namespace selecta::core
