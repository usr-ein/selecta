// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>

#include <chrono>
#include <deque>
#include <optional>
#include <utility>

// A QNetworkAccessManager that never touches the network: requests are answered from scripted responses, matched by
// method and a regular expression on the full URL, and every request is recorded for assertions.
namespace selecta::meta::testing {

inline std::pair<QByteArray, QByteArray> header(const char *name, const char *value)
{
    return {QByteArray(name), QByteArray(value)};
}

struct FakeResponse
{
    int status = 200;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;
    // Set to answer with a transport error instead of an HTTP status.
    std::optional<QNetworkReply::NetworkError> networkError;
    bool hang = false;                   // never finishes until aborted
    std::chrono::milliseconds delay{0};  // before the answer starts
    int chunkSize = 0;                   // > 0: the body arrives in chunks of this size, one readyRead each
    std::optional<qint64> contentLength; // overrides the Content-Length header (may lie); default: the body size

    static FakeResponse json(const QByteArray &body, int status = 200)
    {
        FakeResponse response;
        response.status = status;
        response.body = body;
        response.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        return response;
    }
};

struct RecordedRequest
{
    QByteArray method;
    QUrl url;
    QList<std::pair<QByteArray, QByteArray>> headers;
    QByteArray body;

    QByteArray header(QByteArrayView name) const;
};

class FakeNetwork final : public QNetworkAccessManager
{
    Q_OBJECT

public:
    explicit FakeNetwork(QObject *parent = nullptr);

    // A one-shot answer for the next matching request (FIFO per route).
    void enqueue(const QByteArray &method, const QString &urlPattern, FakeResponse response);
    // An answer for every matching request without a one-shot answer queued.
    void setDefault(const QByteArray &method, const QString &urlPattern, FakeResponse response);

    const QList<RecordedRequest> &requests() const { return m_requests; }
    int requestCount() const { return static_cast<int>(m_requests.size()); }
    int countMatching(const QString &urlPattern) const;
    int unmatched() const { return m_unmatched; } // answered 599 because no route matched
    int liveReplies() const;                      // replies not yet finished

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoingData) override;

private:
    struct Route
    {
        QByteArray method;
        QRegularExpression pattern;
        std::deque<FakeResponse> once;
        std::optional<FakeResponse> always;
    };
    Route &route(const QByteArray &method, const QString &urlPattern);

    QList<Route> m_routes;
    QList<RecordedRequest> m_requests;
    int m_unmatched = 0;
};

// The reply the FakeNetwork hands out.
class FakeReply final : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(const QNetworkRequest &request, const QByteArray &method, FakeResponse response, QObject *parent);

    void abort() override;
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override;

private:
    void begin();
    void deliverChunk();
    void complete();

    FakeResponse m_response;
    QByteArray m_pending; // body not yet delivered
    QByteArray m_buffer;  // delivered, not yet read
    bool m_done = false;
};

} // namespace selecta::meta::testing
