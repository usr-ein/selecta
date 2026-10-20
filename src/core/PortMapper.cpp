// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PortMapper.h"

#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QPromise>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {

namespace {

Q_LOGGING_CATEGORY(lcPortMapper, "selecta.core.portmap")

using portmap::Mapping;
using portmap::MapResult;
using portmap::Outcome;
using State = PortMappingStatus::State;

const char *protocolName(portmap::Protocol protocol)
{
    return protocol == portmap::Protocol::NatPmp ? "NAT-PMP" : "UPnP";
}

} // namespace

PortMapper::PortMapper(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_retryTimer(new QTimer(this))
    , m_renewTimer(new QTimer(this))
{
    qRegisterMetaType<PortMappingStatus>();
    m_pool.setMaxThreadCount(1);
    m_pool.setObjectName(u"selecta-portmap"_s);
    m_retryTimer->setSingleShot(true);
    m_renewTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &PortMapper::reconsider);
    connect(m_renewTimer, &QTimer::timeout, this, [this] {
        if (m_attempt == nullptr && m_mapping) {
            startAttempt(m_mapping);
        }
    });
    m_status.state = State::Off;
}

PortMapper::~PortMapper()
{
    m_retryTimer->stop();
    m_renewTimer->stop();
    if (m_cancel) {
        m_cancel->store(true); // an attempt in flight stops within about 100 ms
    }
    m_pool.waitForDone();
    // An attempt that mapped just as it was cancelled hands back a mapping nobody has seen yet.
    if (m_attempt != nullptr && m_attempt->future().resultCount() > 0) {
        const MapResult result = m_attempt->future().result();
        if (result.outcome == Outcome::Mapped && result.mapping != m_mapping) {
            remove(result.mapping, m_options.quitTiming);
        }
    }
    if (m_mapping) {
        remove(*m_mapping, m_options.quitTiming);
    }
    m_pool.waitForDone();
}

void PortMapper::configure(bool enabled, quint16 port)
{
    if (enabled == m_enabled && port == m_port) {
        return;
    }
    if (port != m_port || (enabled && !m_enabled)) {
        // A new port, or the option turned on again: a fresh set of attempts.
        m_failures = 0;
        m_retryTimer->stop();
    }
    m_enabled = enabled;
    m_port = port;
    reconsider();
}

void PortMapper::reconsider()
{
    const bool wanted = m_enabled && m_port != 0;
    if (m_attempt != nullptr) {
        if (!wanted || m_attemptPort != m_port) {
            m_cancel->store(true); // attemptFinished() reconsiders
        }
        return;
    }
    if (m_mapping && (!wanted || m_mapping->internalPort != m_port)) {
        remove(*m_mapping, m_options.timing);
        m_mapping.reset();
        m_renewTimer->stop();
    }
    if (!wanted) {
        m_retryTimer->stop();
        PortMappingStatus status;
        status.state = m_enabled ? State::Idle : State::Off;
        status.internalPort = m_port;
        publish(status);
        return;
    }
    if (m_mapping || m_retryTimer->isActive() || m_failures >= maxAttempts()) {
        return;
    }
    startAttempt(std::nullopt);
}

void PortMapper::startAttempt(const std::optional<Mapping> &renew)
{
    m_attemptPort = m_port;
    m_cancel = std::make_shared<portmap::Cancel>(false);
    if (!renew) {
        PortMappingStatus status;
        status.state = State::Searching;
        status.internalPort = m_port;
        publish(status);
    }
    portmap::MapRequest request;
    request.internalPort = m_port;
    request.ssdp = m_options.ssdp;
    request.bindAddress = m_options.bindAddress;
    request.natPmpLifetime = m_options.natPmpLifetime;
    request.upnpLease = m_options.upnpLease;
    request.description = m_options.description;
    request.timing = m_options.timing;
    request.renew = renew;

    // The watcher is a child of this object: a result that arrives after it's gone goes nowhere (the destructor reads
    // the last one itself).
    const auto promise = std::make_shared<QPromise<MapResult>>();
    auto *watcher = new QFutureWatcher<MapResult>(this);
    m_attempt = watcher;
    const bool renewal = renew.has_value();
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, renewal] {
        MapResult result;
        result.outcome = Outcome::Cancelled;
        if (watcher->future().resultCount() > 0) {
            result = watcher->result();
        }
        if (m_attempt == watcher) {
            m_attempt = nullptr;
        }
        watcher->deleteLater();
        attemptFinished(result, renewal);
    });
    watcher->setFuture(promise->future());
    promise->start();
    m_pool.start([promise, request = std::move(request), gateway = m_options.gateway,
                  natPmpPort = m_options.natPmpPort, cancel = m_cancel]() mutable {
        if (!request.renew && gateway) {
            if (const std::optional<QHostAddress> address = gateway()) {
                request.natPmpGateway = portmap::Endpoint{*address, natPmpPort};
            }
        }
        promise->addResult(portmap::mapPort(request, *cancel));
        promise->finish();
    });
}

void PortMapper::attemptFinished(const MapResult &result, bool renewal)
{
    const bool wanted = m_enabled && m_port != 0 && m_attemptPort == m_port;
    switch (result.outcome) {
    case Outcome::Mapped: {
        if (!wanted) {
            // Made just as the port changed or the option was turned off.
            remove(result.mapping, m_options.timing);
            if (renewal) {
                m_mapping.reset();
            }
            break;
        }
        const Mapping &mapping = result.mapping;
        if (!renewal || !m_mapping || m_mapping->externalPort != mapping.externalPort) {
            qCInfo(lcPortMapper).noquote() << "port" << mapping.internalPort << "is open on the router as"
                                           << mapping.externalPort << "with" << protocolName(mapping.protocol);
        }
        m_mapping = mapping;
        m_failures = 0;
        scheduleRenewal(mapping);
        PortMappingStatus status;
        status.state = State::Mapped;
        status.protocol = mapping.protocol;
        status.internalPort = mapping.internalPort;
        status.externalPort = mapping.externalPort;
        const QHostAddress &external = mapping.externalAddress;
        if (!external.isNull() && external != QHostAddress(QHostAddress::AnyIPv4)) {
            status.externalAddress = external.toString();
            status.behindAnotherNat = portmap::isUnroutable(external);
        }
        publish(status);
        break;
    }
    case Outcome::Cancelled:
        break;
    case Outcome::NoRouter:
    case Outcome::Refused: {
        if (renewal) {
            m_mapping.reset(); // as far as we know, the router no longer has it
            m_renewTimer->stop();
        }
        if (!wanted) {
            break;
        }
        ++m_failures;
        const bool last = m_failures >= maxAttempts();
        PortMappingStatus status;
        status.state = result.outcome == Outcome::Refused ? State::Refused : State::NoRouter;
        status.reason = result.reason;
        status.internalPort = m_port;
        status.givenUp = last;
        qCInfo(lcPortMapper).noquote() << (renewal ? "renewing the mapping of port" : "opening port") << m_port
                                       << "failed:"
                                       << (result.reason.isEmpty() ? u"no router answered"_s : result.reason)
                                       << (last ? "- giving up for this session" : "");
        publish(status);
        if (!last) {
            m_retryTimer->start(m_options.retryDelays.at(m_failures - 1));
        }
        break;
    }
    }
    reconsider();
}

void PortMapper::remove(const Mapping &mapping, const portmap::Timing &timing)
{
    m_pool.start([mapping, timing, bindAddress = m_options.bindAddress] {
        const portmap::Cancel never{false};
        if (portmap::removeMapping(mapping, bindAddress, timing, never)) {
            qCInfo(lcPortMapper) << "port" << mapping.externalPort << "is closed on the router again";
        } else {
            qCInfo(lcPortMapper) << "the router didn't confirm closing port" << mapping.externalPort;
        }
    });
}

void PortMapper::scheduleRenewal(const Mapping &mapping)
{
    using std::chrono::milliseconds;
    milliseconds interval = m_options.permanentRenewal;
    if (mapping.lifetime.count() > 0) {
        interval = std::max(m_options.minRenewal, milliseconds(mapping.lifetime) / 2);
    }
    m_renewTimer->start(std::min<milliseconds>(interval, std::chrono::hours(24)));
}

void PortMapper::publish(PortMappingStatus status)
{
    if (status == m_status) {
        return;
    }
    m_status = std::move(status);
    Q_EMIT statusChanged(m_status);
}

} // namespace selecta::core
