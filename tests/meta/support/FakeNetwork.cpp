// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/support/FakeNetwork.h"

#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace selecta::meta::testing {

QByteArray RecordedRequest::header(QByteArrayView name) const
{
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return {};
}

FakeNetwork::FakeNetwork(QObject *parent)
    : QNetworkAccessManager(parent)
{}

FakeNetwork::Route &FakeNetwork::route(const QByteArray &method, const QString &urlPattern)
{
    for (Route &existing : m_routes) {
        if (existing.method == method && existing.pattern.pattern() == urlPattern) {
            return existing;
        }
    }
    m_routes.append(Route{method, QRegularExpression(urlPattern), {}, std::nullopt});
    return m_routes.last();
}

void FakeNetwork::enqueue(const QByteArray &method, const QString &urlPattern, FakeResponse response)
{
    route(method, urlPattern).once.push_back(std::move(response));
}

void FakeNetwork::setDefault(const QByteArray &method, const QString &urlPattern, FakeResponse response)
{
    route(method, urlPattern).always = std::move(response);
}

int FakeNetwork::countMatching(const QString &urlPattern) const
{
    const QRegularExpression pattern(urlPattern);
    return static_cast<int>(std::ranges::count_if(
        m_requests, [&](const RecordedRequest &request) { return pattern.match(request.url.toString()).hasMatch(); }));
}

int FakeNetwork::liveReplies() const
{
    const QList<FakeReply *> replies = findChildren<FakeReply *>();
    return static_cast<int>(
        std::ranges::count_if(replies, [](const FakeReply *reply) { return !reply->isFinished(); }));
}

QNetworkReply *FakeNetwork::createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoingData)
{
    QByteArray method;
    switch (operation) {
    case GetOperation:
        method = "GET";
        break;
    case PostOperation:
        method = "POST";
        break;
    case PutOperation:
        method = "PUT";
        break;
    case DeleteOperation:
        method = "DELETE";
        break;
    case HeadOperation:
        method = "HEAD";
        break;
    case CustomOperation:
        method = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        break;
    case UnknownOperation:
        method = "UNKNOWN";
        break;
    }
    RecordedRequest recorded;
    recorded.method = method;
    recorded.url = request.url();
    const QList<QByteArray> names = request.rawHeaderList();
    for (const QByteArray &name : names) {
        recorded.headers.append({name, request.rawHeader(name)});
    }
    if (outgoingData != nullptr) {
        recorded.body = outgoingData->readAll();
    }
    m_requests.append(recorded);

    const QString url = request.url().toString();
    for (Route &route : m_routes) {
        if (route.method != method || !route.pattern.match(url).hasMatch()) {
            continue;
        }
        if (!route.once.empty()) {
            FakeResponse response = std::move(route.once.front());
            route.once.pop_front();
            return new FakeReply(request, method, std::move(response), this);
        }
        if (route.always.has_value()) {
            return new FakeReply(request, method, *route.always, this);
        }
    }
    ++m_unmatched;
    FakeResponse missing;
    missing.status = 599;
    missing.body = "no fake route for " + method + ' ' + url.toUtf8();
    return new FakeReply(request, method, std::move(missing), this);
}

FakeReply::FakeReply(const QNetworkRequest &request, const QByteArray &method, FakeResponse response, QObject *parent)
    : QNetworkReply(parent)
    , m_response(std::move(response))
{
    setRequest(request);
    setUrl(request.url());
    setOperation(QNetworkAccessManager::CustomOperation);
    setAttribute(QNetworkRequest::CustomVerbAttribute, method);
    QNetworkReply::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!m_response.hang) {
        QTimer::singleShot(m_response.delay, this, &FakeReply::begin);
    }
}

void FakeReply::begin()
{
    if (m_done) {
        return;
    }
    if (m_response.networkError.has_value()) {
        setError(*m_response.networkError, u"fake transport error"_s);
        Q_EMIT errorOccurred(*m_response.networkError);
        complete();
        return;
    }
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_response.status);
    for (const auto &[name, value] : std::as_const(m_response.headers)) {
        setRawHeader(name, value);
    }
    setHeader(QNetworkRequest::ContentLengthHeader, m_response.contentLength.value_or(m_response.body.size()));
    Q_EMIT metaDataChanged();
    if (m_done) {
        return; // aborted from a metaDataChanged handler
    }
    m_pending = m_response.body;
    deliverChunk();
}

void FakeReply::deliverChunk()
{
    if (m_done) {
        return;
    }
    if (m_pending.isEmpty()) {
        complete();
        return;
    }
    const qsizetype size =
        m_response.chunkSize > 0 ? std::min<qsizetype>(m_response.chunkSize, m_pending.size()) : m_pending.size();
    m_buffer += m_pending.left(size);
    m_pending.remove(0, size);
    Q_EMIT readyRead();
    if (m_done) {
        return; // aborted from a readyRead handler
    }
    QTimer::singleShot(0, this, &FakeReply::deliverChunk);
}

void FakeReply::complete()
{
    if (m_done) {
        return;
    }
    m_done = true;
    setFinished(true);
    Q_EMIT finished();
}

void FakeReply::abort()
{
    if (m_done) {
        return;
    }
    setError(QNetworkReply::OperationCanceledError, u"Operation canceled"_s);
    Q_EMIT errorOccurred(QNetworkReply::OperationCanceledError);
    complete();
}

qint64 FakeReply::bytesAvailable() const
{
    return m_buffer.size() + QNetworkReply::bytesAvailable();
}

qint64 FakeReply::readData(char *data, qint64 maxSize)
{
    const qint64 n = std::min<qint64>(maxSize, m_buffer.size());
    if (n <= 0) {
        return 0;
    }
    std::memcpy(data, m_buffer.constData(), static_cast<std::size_t>(n));
    m_buffer.remove(0, n);
    return n;
}

} // namespace selecta::meta::testing
