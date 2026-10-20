// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <chrono>
#include <optional>

namespace selecta::devnet {

// How one peer treats download requests, beyond the network-wide defaults (NetworkOptions): so the queue and the
// failure paths of a client show up on demand.
struct PeerBehaviour
{
    std::optional<std::chrono::seconds> queue; // keep each queued download this long before uploading it
    QString refuse;                            // refuse every download with this reason (UploadDenied)

    bool isDefault() const { return !queue && refuse.isEmpty(); }
    friend bool operator==(const PeerBehaviour &, const PeerBehaviour &) = default;
};

using Behaviours = QHash<QString, PeerBehaviour>; // by peer name

// The file a library folder may hold to set its peers' behaviour (--generate writes one).
QString behaviourFileName();

// Lines of a behaviour file: "<peer> queue <seconds>" or "<peer> refuse <reason…>"; blank lines and lines starting
// with '#' are skipped. A line that doesn't parse is reported in `errors`, with its number, and skipped.
Behaviours parseBehaviours(const QString &text, QStringList *errors);

// Command-line values: "<peer>=<seconds>" for --queue, "<peer>" or "<peer>=<reason>" for --refuse (default reason
// "Too many files"). False when the value doesn't parse.
bool addQueueOption(Behaviours &behaviours, const QString &value);
bool addRefuseOption(Behaviours &behaviours, const QString &value);

// "queues each download 45 s", "refuses every download: Too many files", or "default".
QString describeBehaviour(const PeerBehaviour &behaviour);

} // namespace selecta::devnet
