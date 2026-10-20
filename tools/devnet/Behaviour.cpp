// SPDX-License-Identifier: GPL-3.0-or-later
#include "Behaviour.h"

#include "slsk/PeerMessages.h"

using namespace Qt::StringLiterals;

namespace selecta::devnet {
namespace {

constexpr qint64 kLongestQueueSeconds = qint64{24} * 3600;

std::optional<std::chrono::seconds> seconds(const QString &text)
{
    bool ok = false;
    const qint64 value = text.trimmed().toLongLong(&ok);
    if (!ok || value < 0 || value > kLongestQueueSeconds) {
        return std::nullopt;
    }
    return std::chrono::seconds(value);
}

} // namespace

QString behaviourFileName()
{
    return u"devnet.conf"_s;
}

Behaviours parseBehaviours(const QString &text, QStringList *errors)
{
    Behaviours behaviours;
    const QStringList lines = text.split(u'\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        const QStringList fields = line.split(u' ', Qt::SkipEmptyParts);
        const auto fail = [&] {
            if (errors != nullptr) {
                errors->append(u"line %1: %2"_s.arg(i + 1).arg(line));
            }
        };
        if (fields.size() < 3 || !slsk::isAcceptablePeerUsername(fields.at(0))) {
            fail();
            continue;
        }
        PeerBehaviour behaviour = behaviours.value(fields.at(0));
        bool understood = true;
        if (fields.at(1) == u"queue"_s && fields.size() == 3) {
            behaviour.queue = seconds(fields.at(2));
            understood = behaviour.queue.has_value();
        } else if (fields.at(1) == u"refuse"_s) {
            behaviour.refuse = fields.mid(2).join(u' ');
        } else {
            understood = false;
        }
        if (understood) {
            behaviours.insert(fields.at(0), behaviour);
        } else {
            fail();
        }
    }
    return behaviours;
}

bool addQueueOption(Behaviours &behaviours, const QString &value)
{
    const qsizetype equals = value.indexOf(u'=');
    const QString peer = value.left(equals);
    const std::optional<std::chrono::seconds> wait = equals > 0 ? seconds(value.mid(equals + 1)) : std::nullopt;
    if (!wait || !slsk::isAcceptablePeerUsername(peer)) {
        return false;
    }
    behaviours[peer].queue = wait;
    return true;
}

bool addRefuseOption(Behaviours &behaviours, const QString &value)
{
    const qsizetype equals = value.indexOf(u'=');
    const QString peer = equals < 0 ? value : value.left(equals);
    const QString reason = equals < 0 ? QString() : value.mid(equals + 1).trimmed();
    if (!slsk::isAcceptablePeerUsername(peer)) {
        return false;
    }
    behaviours[peer].refuse = reason.isEmpty() ? u"Too many files"_s : reason;
    return true;
}

QString describeBehaviour(const PeerBehaviour &behaviour)
{
    if (!behaviour.refuse.isEmpty()) {
        return u"refuses every download: %1"_s.arg(behaviour.refuse);
    }
    if (behaviour.queue) {
        return u"queues each download %1 s"_s.arg(behaviour.queue->count());
    }
    return u"default"_s;
}

} // namespace selecta::devnet
