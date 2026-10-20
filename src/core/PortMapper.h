// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/PortMapClient.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QTimer;
template <typename T>
class QFutureWatcher;

namespace selecta::core {

// Whether the listening port is open on the router (PRD NET-3), for Settings → Soulseek.
struct PortMappingStatus
{
    enum class State : quint8 {
        Unavailable, // this run never touches the router (isolated runs without --port-mapping, tests)
        Off,         // the user turned it off
        Idle,        // on, but Selecta isn't listening for peers yet
        Searching,   // looking for the router
        Mapped,
        NoRouter, // no router answered
        Refused,  // a router answered no; `reason` says what it said
    };
    State state = State::Unavailable;
    portmap::Protocol protocol = portmap::Protocol::NatPmp; // Mapped
    QString externalAddress;                                // Mapped, when the router said
    quint16 externalPort = 0;                               // Mapped
    quint16 internalPort = 0;                               // the listening port it is about
    bool behindAnotherNat = false; // Mapped, but the router's own address is private: peers may still not reach us
    QString reason;                // Refused
    bool givenUp = false;          // NoRouter or Refused after the last attempt of this session

    bool operator==(const PortMappingStatus &) const = default;
};

// Opens the listening port on the router (PRD NET-3): NAT-PMP to the default gateway first, then UPnP IGD. The mapping
// keeps the same external port when the router has it free, is renewed at half its lifetime, and is removed when the
// port changes, when the user turns it off, and when the mapper is destroyed (at quit).
//
// A failed attempt is retried after Options::retryDelays, then the mapper gives up quietly for the session; a new
// port, or turning the option off and on, starts over. A failed renewal counts as a failed attempt.
//
// Threading: lives on the engine thread and is used only from it. The protocol exchanges run on its own one-thread
// pool, so nothing blocks the engine thread except the destructor, which removes the mapping and waits for that
// (bounded by Options::quitTiming: about 1.5 s at worst). Ownership: a QObject; owns its pool and timers.
// Failures: never fatal, always reported in status(); details go to the log (selecta.core.portmap).
class PortMapper : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        // The NAT-PMP gateway, looked up on the worker before each attempt (nullopt: skip NAT-PMP); tests return a
        // loopback fake. natPmpPort is RFC 6886's 5351 except in tests.
        std::function<std::optional<QHostAddress>()> gateway = portmap::defaultGateway;
        quint16 natPmpPort = portmap::kNatPmpPort;
        // Where SSDP searches go (nullopt: skip UPnP); tests use a loopback fake.
        std::optional<portmap::Endpoint> ssdp = portmap::ssdpMulticast();
        QHostAddress bindAddress{QHostAddress::AnyIPv4};
        portmap::Timing timing;
        // Removing the mapping at quit: short, since quitting waits for it.
        portmap::Timing quitTiming{std::chrono::milliseconds(250), 2, std::chrono::milliseconds(0),
                                   std::chrono::milliseconds(1500)};
        std::chrono::seconds natPmpLifetime{7200};
        std::chrono::seconds upnpLease{3600};
        // Never renews more often than this, whatever lifetime the router grants.
        std::chrono::milliseconds minRenewal{std::chrono::minutes(1)};
        // A permanent UPnP lease is asserted again this often: a router may forget it when it restarts.
        std::chrono::milliseconds permanentRenewal{std::chrono::minutes(30)};
        // The wait after each failed attempt; after the last one the mapper gives up for the session.
        QList<std::chrono::milliseconds> retryDelays{std::chrono::seconds(30), std::chrono::minutes(2)};
        QString description = QStringLiteral("Selecta");
    };

    explicit PortMapper(Options options, QObject *parent = nullptr);
    // Removes the mapping, if any, and waits for the worker (bounded: see the class comment).
    ~PortMapper() override;
    PortMapper(const PortMapper &) = delete;
    PortMapper &operator=(const PortMapper &) = delete;
    PortMapper(PortMapper &&) = delete;
    PortMapper &operator=(PortMapper &&) = delete;

    // The user's setting and the port Selecta listens on (0: not listening). Either change takes effect at once.
    void configure(bool enabled, quint16 port);

    PortMappingStatus status() const { return m_status; }
    // The mapping on the router, if any.
    std::optional<portmap::Mapping> mapping() const { return m_mapping; }

Q_SIGNALS:
    void statusChanged(const selecta::core::PortMappingStatus &status);

private:
    void reconsider();
    void startAttempt(const std::optional<portmap::Mapping> &renew);
    void attemptFinished(const portmap::MapResult &result, bool renewal);
    void remove(const portmap::Mapping &mapping, const portmap::Timing &timing);
    void scheduleRenewal(const portmap::Mapping &mapping);
    void publish(PortMappingStatus status);
    int maxAttempts() const { return static_cast<int>(m_options.retryDelays.size()) + 1; }

    Options m_options;
    bool m_enabled = false;
    quint16 m_port = 0;
    std::optional<portmap::Mapping> m_mapping;
    int m_failures = 0;                                      // failed attempts in a row
    quint16 m_attemptPort = 0;                               // the port the attempt in flight maps
    QFutureWatcher<portmap::MapResult> *m_attempt = nullptr; // the attempt in flight
    std::shared_ptr<portmap::Cancel> m_cancel;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_renewTimer = nullptr;
    QThreadPool m_pool; // one thread: attempts and removals run in the order they were asked for
    PortMappingStatus m_status;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::PortMappingStatus)
