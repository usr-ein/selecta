// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QProcess>
#include <QString>
#include <QTemporaryDir>

namespace selecta::slsk::testing {

// A port nobody listens on right now.
quint16 freePort();

// The pinned Soulfind (devenv/fetch-soulfind.sh), or SELECTA_SOULFIND; empty when neither is there.
QString soulfindBinary();

// Whether the devenv tests may run (SELECTA_DEVENV=1); they skip otherwise.
bool devenvEnabled();

// One test's Soulfind: a free port, a database in a temporary folder, stopped with the object. With
// SELECTA_SOULFIND_DEBUG=1 it runs with --debug and prints its output when it stops.
class Soulfind
{
public:
    Soulfind() = default;
    ~Soulfind();
    Soulfind(const Soulfind &) = delete;
    Soulfind &operator=(const Soulfind &) = delete;
    Soulfind(Soulfind &&) = delete;
    Soulfind &operator=(Soulfind &&) = delete;

    // Starts it and waits until it accepts connections on 127.0.0.1. Empty on success, else why not.
    QString start(const QString &binary);
    quint16 port() const { return m_port; }

private:
    QProcess m_process;
    QTemporaryDir m_data;
    quint16 m_port = 0;
};

} // namespace selecta::slsk::testing
