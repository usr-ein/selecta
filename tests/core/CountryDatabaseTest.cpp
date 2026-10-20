// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CountryDatabase.h"
#include "core/CountryDatabaseFetcher.h"

#include "MmdbWriter.h"

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::CountryDatabase;
using selecta::core::CountryDatabaseFetcher;
using selecta::core::CountryDatabaseOptions;
using selecta::core::testing::FakeHttpServer;
using selecta::core::testing::MmdbWriter;

namespace {

QByteArray sampleDatabase(int ipVersion = 6)
{
    MmdbWriter writer(ipVersion);
    writer.add(u"81.2.69.0"_s, 24, "GB");
    writer.add(u"89.160.20.0"_s, 24, "SE");
    writer.add(u"175.16.199.0"_s, 24, "CN");
    writer.add(u"203.0.113.0"_s, 24, "ZZ");  // not a territory
    writer.add(u"198.51.100.0"_s, 24, "fr"); // lower case: refused
    writer.add(u"192.0.2.0"_s, 24, "FRA");   // three letters: refused
    writer.addWithoutCountry(u"5.5.5.0"_s, 24);
    if (ipVersion == 6) {
        writer.add(u"2a02:ec0::"_s, 32, "DE");
    }
    return writer.build();
}

QString write(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    return path;
}

} // namespace

TEST(CountryDatabaseTest, LooksPeersUpByAddress)
{
    const QTemporaryDir dir;
    QString error;
    const auto database = CountryDatabase::open(write(dir, u"c.mmdb"_s, sampleDatabase()), &error);
    ASSERT_TRUE(database) << error.toStdString();
    EXPECT_EQ(database->countryOf(QHostAddress(u"81.2.69.160"_s)), u"GB"_s);
    EXPECT_EQ(database->countryOf(QHostAddress(u"89.160.20.1"_s)), u"SE"_s);
    EXPECT_EQ(database->countryOf(QHostAddress(u"::ffff:175.16.199.4"_s)), u"CN"_s); // v4-mapped, as sockets give
    EXPECT_EQ(database->countryOf(QHostAddress(u"2a02:ec0::1"_s)), u"DE"_s);
    // Unknown: not in the file, no country in the record, or not a real code.
    EXPECT_TRUE(database->countryOf(QHostAddress(u"8.8.8.8"_s)).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress(u"5.5.5.5"_s)).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress(u"203.0.113.9"_s)).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress(u"198.51.100.9"_s)).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress(u"192.0.2.9"_s)).isEmpty());
    // Never looked up: no address, this machine, a private network.
    EXPECT_TRUE(database->countryOf(QHostAddress()).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress::LocalHost).isEmpty());
    EXPECT_TRUE(database->countryOf(QHostAddress(u"192.168.1.20"_s)).isEmpty());
}

TEST(CountryDatabaseTest, AnIpv4DatabaseIgnoresIpv6Addresses)
{
    const QTemporaryDir dir;
    QString error;
    const auto database = CountryDatabase::open(write(dir, u"c.mmdb"_s, sampleDatabase(4)), &error);
    ASSERT_TRUE(database) << error.toStdString();
    EXPECT_EQ(database->countryOf(QHostAddress(u"81.2.69.160"_s)), u"GB"_s);
    EXPECT_TRUE(database->countryOf(QHostAddress(u"2a02:ec0::1"_s)).isEmpty());
}

TEST(CountryDatabaseTest, RefusesFilesThatArentACountryDatabase)
{
    const QTemporaryDir dir;
    QString error;
    EXPECT_FALSE(CountryDatabase::open(dir.filePath(u"missing.mmdb"_s), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"empty.mmdb"_s, {}), &error));
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"junk.mmdb"_s, QByteArray(4096, 'x')), &error));
    const QByteArray good = sampleDatabase();
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"half.mmdb"_s, good.left(good.size() / 2)), &error));
    // Another kind of MaxMind database.
    MmdbWriter city(6, "GeoLite2-ASN");
    city.add(u"81.2.69.0"_s, 24, "GB");
    error.clear();
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"asn.mmdb"_s, city.build()), &error));
    EXPECT_TRUE(error.contains(u"country"_s)) << error.toStdString();
    // Larger than the cap.
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"big.mmdb"_s, good), &error, good.size() - 1));
    EXPECT_TRUE(CountryDatabase::open(dir.filePath(u"big.mmdb"_s), &error, good.size()));
    // A record pointing past the data, on the way to a sample address.
    MmdbWriter broken;
    broken.addBroken(u"8.8.8.0"_s, 24);
    error.clear();
    EXPECT_FALSE(CountryDatabase::open(write(dir, u"broken.mmdb"_s, broken.build()), &error));
    EXPECT_TRUE(error.contains(u"damaged"_s)) << error.toStdString();
}

TEST(CountryDatabaseTest, CountryCodesAreRealTerritories)
{
    EXPECT_TRUE(selecta::core::isCountryCode(u"FR"));
    EXPECT_TRUE(selecta::core::isCountryCode(u"JP"));
    EXPECT_FALSE(selecta::core::isCountryCode(u"fr"));
    EXPECT_FALSE(selecta::core::isCountryCode(u"ZZ"));
    EXPECT_FALSE(selecta::core::isCountryCode(u"F"));
    EXPECT_FALSE(selecta::core::isCountryCode(u"FRA"));
    EXPECT_FALSE(selecta::core::isCountryCode(u""));
    EXPECT_FALSE(selecta::core::isCountryCode(u"F1"));
}

TEST(CountryDatabaseTest, GunzipIsBounded)
{
    const QByteArray raw = sampleDatabase();
    const QByteArray packed = MmdbWriter::gzip(raw);
    QString error;
    EXPECT_EQ(CountryDatabaseFetcher::gunzip(packed, raw.size(), &error), raw);
    // A bomb: inflating past the cap stops at the cap.
    EXPECT_TRUE(CountryDatabaseFetcher::gunzip(MmdbWriter::gzip(QByteArray(1'000'000, '\0')), 1000, &error).isEmpty());
    EXPECT_TRUE(error.contains(u"past"_s)) << error.toStdString();
    EXPECT_TRUE(CountryDatabaseFetcher::gunzip(packed.left(packed.size() - 9), raw.size(), &error).isEmpty());
    EXPECT_TRUE(CountryDatabaseFetcher::gunzip(packed + "extra", raw.size(), &error).isEmpty());
    EXPECT_TRUE(error.contains(u"follows"_s)) << error.toStdString();
    EXPECT_TRUE(CountryDatabaseFetcher::gunzip("not gzip at all", raw.size(), &error).isEmpty());
    EXPECT_TRUE(CountryDatabaseFetcher::gunzip({}, raw.size(), &error).isEmpty());
}

TEST(CountryDatabaseTest, SourcesAreThisMonthAndTheMonthBefore)
{
    const QList<QUrl> sources = selecta::core::dbIpCountryLiteSources(QDate(2026, 1, 3));
    ASSERT_EQ(sources.size(), 2);
    EXPECT_EQ(sources.at(0).toString(), u"https://download.db-ip.com/free/dbip-country-lite-2026-01.mmdb.gz"_s);
    EXPECT_EQ(sources.at(1).toString(), u"https://download.db-ip.com/free/dbip-country-lite-2025-12.mmdb.gz"_s);
    EXPECT_TRUE(selecta::core::dbIpCountryLiteSources(QDate()).isEmpty());
}

class CountryDatabaseFetcherTest : public ::testing::Test
{
protected:
    CountryDatabaseOptions options(const QList<QUrl> &sources) const
    {
        return {.path = m_dir.filePath(u"GeoIP/countries.mmdb"_s), .sources = sources, .stallMs = 2'000, .totalMs = 10'000};
    }

    // Runs a fetcher to its end; the database when it was ready, null when it failed.
    std::shared_ptr<const CountryDatabase> run(const CountryDatabaseOptions &options, QString *why = nullptr)
    {
        CountryDatabaseFetcher fetcher(options, m_network);
        QSignalSpy ready(&fetcher, &CountryDatabaseFetcher::ready);
        QSignalSpy failed(&fetcher, &CountryDatabaseFetcher::failed);
        fetcher.start();
        fetcher.start(); // once only
        EXPECT_TRUE(QTest::qWaitFor([&] { return !ready.isEmpty() || !failed.isEmpty(); }, 15'000));
        QTest::qWait(50); // nothing more comes
        EXPECT_EQ(ready.size() + failed.size(), 1);
        if (!failed.isEmpty() && why != nullptr) {
            *why = failed.constFirst().constFirst().toString();
        }
        return ready.isEmpty() ? nullptr
                               : ready.constFirst().constFirst().value<std::shared_ptr<const CountryDatabase>>();
    }

    QTemporaryDir m_dir;
    QNetworkAccessManager m_network;
    FakeHttpServer m_server;
};

TEST_F(CountryDatabaseFetcherTest, DownloadsOnceWhenMissingThenOpensFromDisk)
{
    m_server.answer(u"/db.gz"_s, {.status = 200, .body = MmdbWriter::gzip(sampleDatabase())});
    const CountryDatabaseOptions where = options({m_server.url(u"/db.gz"_s)});
    const auto database = run(where);
    ASSERT_TRUE(database);
    EXPECT_EQ(database->countryOf(QHostAddress(u"81.2.69.1"_s)), u"GB"_s);
    EXPECT_TRUE(QFile::exists(where.path));
    EXPECT_FALSE(QFile::exists(where.path + u".part"_s));
    EXPECT_EQ(m_server.totalRequests(), 1);

    // The next launch reads the file and asks nothing.
    ASSERT_TRUE(run(where));
    EXPECT_EQ(m_server.totalRequests(), 1);
}

TEST_F(CountryDatabaseFetcherTest, FallsBackToTheMonthBeforeOnce)
{
    m_server.answer(u"/old.gz"_s, {.status = 200, .body = MmdbWriter::gzip(sampleDatabase())});
    ASSERT_TRUE(run(options({m_server.url(u"/new.gz"_s), m_server.url(u"/old.gz"_s)})));
    EXPECT_EQ(m_server.requests(u"/new.gz"_s), 1); // a 404
    EXPECT_EQ(m_server.requests(u"/old.gz"_s), 1);
}

TEST_F(CountryDatabaseFetcherTest, AFailureMeansNoFlagsAndNoRetry)
{
    m_server.answer(u"/junk.gz"_s, {.status = 200, .body = MmdbWriter::gzip("not a database")});
    m_server.answer(u"/raw"_s, {.status = 200, .body = sampleDatabase()}); // not gzip
    QString why;
    EXPECT_FALSE(run(options({m_server.url(u"/missing.gz"_s), m_server.url(u"/junk.gz"_s), m_server.url(u"/raw"_s)}),
                     &why));
    EXPECT_FALSE(why.isEmpty());
    EXPECT_EQ(m_server.totalRequests(), 3); // each source once
    EXPECT_FALSE(QFile::exists(m_dir.filePath(u"GeoIP/countries.mmdb"_s)));
    EXPECT_FALSE(QFile::exists(m_dir.filePath(u"GeoIP/countries.mmdb.part"_s)));
}

TEST_F(CountryDatabaseFetcherTest, RefusesOversizedDownloads)
{
    m_server.answer(u"/huge.gz"_s, {.status = 200,
                                    .body = MmdbWriter::gzip(sampleDatabase()),
                                    .hang = false,
                                    .claimedLength = CountryDatabaseFetcher::kMaxDownloadBytes + 1});
    QString why;
    EXPECT_FALSE(run(options({m_server.url(u"/huge.gz"_s)}), &why));
    EXPECT_TRUE(why.contains(u"larger"_s)) << why.toStdString();
}

TEST_F(CountryDatabaseFetcherTest, GivesUpOnAStalledServer)
{
    m_server.answer(u"/slow.gz"_s, {.status = 200, .body = {}, .hang = true, .claimedLength = -1});
    CountryDatabaseOptions where = options({m_server.url(u"/slow.gz"_s)});
    where.stallMs = 200;
    EXPECT_FALSE(run(where));

    where.stallMs = 5'000;
    where.totalMs = 300;
    QString why;
    EXPECT_FALSE(run(where, &why));
    EXPECT_TRUE(why.contains(u"longer"_s)) << why.toStdString();
}

TEST_F(CountryDatabaseFetcherTest, OnlyHttpsOrLoopback)
{
    QString why;
    // Refused before any request: nothing leaves the machine.
    EXPECT_FALSE(run(options({QUrl(u"http://download.db-ip.com/free/x.mmdb.gz"_s), QUrl(u"ftp://127.0.0.1/x"_s)}),
                     &why));
    EXPECT_TRUE(why.contains(u"refused"_s)) << why.toStdString();
    EXPECT_FALSE(run(options({}), &why));
}

TEST_F(CountryDatabaseFetcherTest, ReplacesADamagedFileOnce)
{
    QDir().mkpath(m_dir.filePath(u"GeoIP"_s));
    write(m_dir, u"GeoIP/countries.mmdb"_s, "damaged");
    m_server.answer(u"/db.gz"_s, {.status = 200, .body = MmdbWriter::gzip(sampleDatabase())});
    const auto database = run(options({m_server.url(u"/db.gz"_s)}));
    ASSERT_TRUE(database);
    EXPECT_EQ(database->countryOf(QHostAddress(u"89.160.20.9"_s)), u"SE"_s);
    EXPECT_EQ(m_server.totalRequests(), 1);
}

TEST_F(CountryDatabaseFetcherTest, NoPathDoesNothing)
{
    CountryDatabaseFetcher fetcher({}, m_network);
    QSignalSpy ready(&fetcher, &CountryDatabaseFetcher::ready);
    QSignalSpy failed(&fetcher, &CountryDatabaseFetcher::failed);
    fetcher.start();
    QTest::qWait(50);
    EXPECT_TRUE(ready.isEmpty() && failed.isEmpty());
}
