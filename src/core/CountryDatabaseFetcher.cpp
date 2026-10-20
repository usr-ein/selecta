// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CountryDatabaseFetcher.h"

#include "base/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcGeo, "selecta.core.geo")

// https anywhere; plain http only to this machine (the tests' fake server).
bool allowedSource(const QUrl &url)
{
    if (!url.isValid()) {
        return false;
    }
    if (url.scheme() == u"https"_s) {
        return true;
    }
    return url.scheme() == u"http"_s && QHostAddress(url.host()).isLoopback();
}

} // namespace

QList<QUrl> dbIpCountryLiteSources(QDate today)
{
    if (!today.isValid()) {
        return {};
    }
    const auto source = [](QDate month) {
        return QUrl(u"https://download.db-ip.com/free/dbip-country-lite-%1.mmdb.gz"_s.arg(month.toString(u"yyyy-MM"_s)));
    };
    return {source(today), source(today.addMonths(-1))};
}

CountryDatabaseFetcher::CountryDatabaseFetcher(CountryDatabaseOptions options, QNetworkAccessManager &network,
                                               QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_network(network)
{
    m_worker.setMaxThreadCount(1);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(std::max(m_options.totalMs, 1));
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
        }
        fail(u"the download took longer than %1 ms"_s.arg(m_options.totalMs));
    });
}

CountryDatabaseFetcher::~CountryDatabaseFetcher()
{
    m_finished = true;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
    m_worker.waitForDone(); // its results are posted to this object, and dropped once it's gone
}

void CountryDatabaseFetcher::start()
{
    if (m_started || m_options.path.isEmpty()) {
        return;
    }
    m_started = true;
    if (!QFileInfo::exists(m_options.path)) {
        m_deadline.start();
        fetchNext();
        return;
    }
    m_worker.start([this, path = m_options.path] {
        QString error;
        std::shared_ptr<const CountryDatabase> database = CountryDatabase::open(path, &error);
        if (!database) {
            QFile::remove(path); // damaged or foreign: fetched again, once
        }
        QMetaObject::invokeMethod(
            this,
            [this, database = std::move(database), error] {
                if (m_finished) {
                    return;
                }
                if (database) {
                    m_finished = true;
                    Q_EMIT ready(database);
                    return;
                }
                qCWarning(lcGeo).noquote() << "country database set aside:" << error;
                m_deadline.start();
                fetchNext();
            },
            Qt::QueuedConnection);
    });
}

void CountryDatabaseFetcher::fetchNext()
{
    if (m_finished) {
        return;
    }
    while (m_nextSource < m_options.sources.size() && !allowedSource(m_options.sources.at(m_nextSource))) {
        m_lastError = u"refused source %1"_s.arg(m_options.sources.at(m_nextSource).toDisplayString());
        ++m_nextSource;
    }
    if (m_nextSource >= m_options.sources.size()) {
        fail(m_lastError.isEmpty() ? u"no source for the country database"_s : m_lastError);
        return;
    }
    const QUrl url = m_options.sources.at(m_nextSource++);
    QNetworkRequest request(url);
    request.setTransferTimeout(std::max(m_options.stallMs, 1));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (received > kMaxDownloadBytes || total > kMaxDownloadBytes) {
            m_lastError = u"the download is larger than %1 bytes"_s.arg(kMaxDownloadBytes);
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
            fetchNext();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_lastError = u"%1: %2"_s.arg(url.toDisplayString(), reply->errorString());
            fetchNext();
            return;
        }
        const QByteArray body = reply->read(kMaxDownloadBytes + 1);
        if (body.size() > kMaxDownloadBytes || body.isEmpty()) {
            m_lastError = u"%1: %2 bytes"_s.arg(url.toDisplayString()).arg(body.size());
            fetchNext();
            return;
        }
        downloaded(body);
    });
}

void CountryDatabaseFetcher::downloaded(const QByteArray &compressed)
{
    m_worker.start([this, compressed, path = m_options.path] {
        QString error;
        std::shared_ptr<const CountryDatabase> database;
        const QByteArray raw = gunzip(compressed, CountryDatabase::kMaxBytes, &error);
        if (!raw.isEmpty()) {
            const QString part = path + u".part"_s;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile file(part);
            if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size() || !file.commit()) {
                error = u"can't write %1: %2"_s.arg(part, file.errorString());
            } else if ((database = CountryDatabase::open(part, &error))) {
                // The mapping follows the file through the rename.
                QFile::remove(path);
                if (!QFile::rename(part, path)) {
                    error = u"can't move the country database into %1"_s.arg(path);
                    database.reset();
                }
            }
            QFile::remove(part);
        }
        QMetaObject::invokeMethod(
            this,
            [this, database = std::move(database), error] {
                if (m_finished) {
                    return;
                }
                if (database) {
                    m_finished = true;
                    m_deadline.stop();
                    qCInfo(lcGeo) << "country database downloaded";
                    Q_EMIT ready(database);
                    return;
                }
                m_lastError = error;
                fetchNext();
            },
            Qt::QueuedConnection);
    });
}

void CountryDatabaseFetcher::fail(const QString &why)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_deadline.stop();
    qCWarning(lcGeo).noquote() << "no country flags this run:" << why;
    Q_EMIT failed(why);
}

QByteArray CountryDatabaseFetcher::gunzip(const QByteArray &compressed, qint64 maxOutput, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return QByteArray();
    };
    if (compressed.isEmpty() || compressed.size() > std::numeric_limits<uInt>::max()) {
        return fail(u"nothing to inflate"_s);
    }
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) { // 16: gzip only
        return fail(u"zlib failed to start"_s);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib's input pointer isn't const, it doesn't write to it
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    QByteArray out;
    std::array<char, 64 * 1024> chunk{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            return fail(status == Z_BUF_ERROR ? u"the file is truncated"_s : u"the file isn't valid gzip"_s);
        }
        const auto produced = static_cast<qint64>(chunk.size() - stream.avail_out);
        if (out.size() + produced > maxOutput) {
            inflateEnd(&stream);
            return fail(u"the database inflates past %1 bytes"_s.arg(maxOutput));
        }
        out.append(chunk.data(), static_cast<qsizetype>(produced));
        if (status == Z_OK && produced == 0 && stream.avail_in == 0) {
            inflateEnd(&stream);
            return fail(u"the file is truncated"_s);
        }
    }
    const bool trailing = stream.avail_in != 0;
    inflateEnd(&stream);
    if (trailing) {
        return fail(u"data follows the gzip stream"_s);
    }
    return out;
}

} // namespace selecta::core
