// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/UploadQueue.h"

#include <algorithm>
#include <limits>

namespace selecta::share {

namespace {

UploadQueue::Options clamped(const UploadQueue::Options &options)
{
    UploadQueue::Options result = options;
    result.slots = std::max(0, result.slots);
    result.maxFilesPerUser = std::clamp(result.maxFilesPerUser, 0, UploadQueue::kMaxEntries);
    result.maxBytesPerUser = std::max<qint64>(0, result.maxBytesPerUser);
    return result;
}

template <typename Entries>
qsizetype indexOf(const Entries &entries, const slsk::RemotePath &path)
{
    const auto it = std::ranges::find_if(entries, [&path](const auto &entry) { return entry.path == path; });
    return it == entries.end() ? -1 : static_cast<qsizetype>(it - entries.begin());
}

UploadQueue::Decision refused(slsk::RejectionReason reason)
{
    return UploadQueue::Decision{false, false, slsk::rejectionText(reason)};
}

} // namespace

UploadQueue::UploadQueue()
    : UploadQueue(Options{})
{}

UploadQueue::UploadQueue(const Options &options)
    : m_options(clamped(options))
{}

void UploadQueue::setOptions(const Options &options)
{
    m_options = clamped(options);
}

UploadQueue::Decision UploadQueue::enqueue(const QString &user, const slsk::RemotePath &path, quint64 size,
                                           bool privileged)
{
    auto it = m_users.find(user);
    if (it != m_users.end()) {
        it->privilegedHint = privileged; // the latest word on it
        if (indexOf(it->queued, path) >= 0 || indexOf(it->active, path) >= 0) {
            return Decision{true, true, {}};
        }
    }
    const qsizetype queuedFiles = it != m_users.end() ? it->queued.size() : 0;
    const quint64 queuedBytes = it != m_users.end() ? it->queuedBytes : 0;
    if (queuedFiles >= m_options.maxFilesPerUser || m_queued + m_active >= kMaxEntries) {
        return refused(slsk::RejectionReason::TooManyFiles);
    }
    if (queuedBytes >= static_cast<quint64>(m_options.maxBytesPerUser)) {
        return refused(slsk::RejectionReason::TooManyMegabytes);
    }
    if (it == m_users.end()) {
        User arrival;
        arrival.name = user;
        arrival.turn = m_clock; // waiting since now
        arrival.arrival = ++m_arrivals;
        arrival.privilegedHint = privileged;
        it = m_users.insert(user, std::move(arrival));
    }
    it->queued.append(Entry{path, size});
    it->queuedBytes += size;
    ++m_queued;
    return Decision{true, false, {}};
}

std::optional<UploadQueue::Next> UploadQueue::next() const
{
    if (m_active >= m_options.slots) {
        return std::nullopt;
    }
    const User *chosen = nullptr;
    for (const User &user : m_users) {
        if (user.queued.isEmpty() || !user.active.isEmpty()) {
            continue;
        }
        if (chosen == nullptr || goesBefore(user, *chosen)) {
            chosen = &user;
        }
    }
    if (chosen == nullptr) {
        return std::nullopt;
    }
    const Entry &entry = chosen->queued.constFirst();
    return Next{chosen->name, entry.path, entry.size};
}

bool UploadQueue::started(const QString &user, const slsk::RemotePath &path)
{
    const auto it = m_users.find(user);
    if (it == m_users.end()) {
        return false;
    }
    const qsizetype index = indexOf(it->queued, path);
    if (index < 0) {
        return false;
    }
    Entry entry = it->queued.takeAt(index);
    it->queuedBytes -= entry.size;
    --m_queued;
    it->active.append(std::move(entry));
    ++m_active;
    it->turn = ++m_clock;
    return true;
}

bool UploadQueue::finished(const QString &user, const slsk::RemotePath &path)
{
    const auto it = m_users.find(user);
    if (it == m_users.end()) {
        return false;
    }
    const qsizetype index = indexOf(it->active, path);
    if (index < 0) {
        return false;
    }
    it->active.removeAt(index);
    --m_active;
    dropIfIdle(it);
    return true;
}

bool UploadQueue::remove(const QString &user, const slsk::RemotePath &path)
{
    const auto it = m_users.find(user);
    if (it == m_users.end()) {
        return false;
    }
    if (const qsizetype index = indexOf(it->queued, path); index >= 0) {
        it->queuedBytes -= it->queued.at(index).size;
        it->queued.removeAt(index);
        --m_queued;
    } else if (const qsizetype active = indexOf(it->active, path); active >= 0) {
        it->active.removeAt(active);
        --m_active;
    } else {
        return false;
    }
    dropIfIdle(it);
    return true;
}

QList<slsk::RemotePath> UploadQueue::removeUser(const QString &user)
{
    const auto it = m_users.find(user);
    if (it == m_users.end()) {
        return {};
    }
    QList<slsk::RemotePath> paths;
    paths.reserve(it->queued.size());
    for (const Entry &entry : std::as_const(it->queued)) {
        paths.append(entry.path);
    }
    m_queued -= static_cast<int>(it->queued.size());
    m_active -= static_cast<int>(it->active.size());
    m_users.erase(it);
    return paths;
}

void UploadQueue::setPrivileged(const QSet<QString> &users)
{
    m_privileged = users;
    for (User &user : m_users) {
        user.privilegedHint = false;
    }
}

std::optional<quint32> UploadQueue::placeInQueue(const QString &user, const slsk::RemotePath &path) const
{
    const auto it = m_users.constFind(user);
    if (it == m_users.cend()) {
        return std::nullopt;
    }
    const qsizetype index = indexOf(it->queued, path);
    if (index < 0) {
        return std::nullopt;
    }
    const User &self = it.value();
    const bool privileged = isPrivileged(self);
    // The file is served in round `index + 1` of its group's turns: count every file served before it.
    const qsizetype round = index + 1;
    qint64 place = round;
    for (const User &other : m_users) {
        if (&other == &self || other.queued.isEmpty()) {
            continue;
        }
        const qsizetype files = other.queued.size();
        const bool otherPrivileged = isPrivileged(other);
        if (otherPrivileged != privileged) {
            place += otherPrivileged ? files : 0; // a privileged user's files all come first
            continue;
        }
        place += goesBefore(other, self) ? std::min(files, round) : std::min(files, round - 1);
    }
    return static_cast<quint32>(std::min<qint64>(place, std::numeric_limits<quint32>::max()));
}

bool UploadQueue::isPrivileged(const User &user) const
{
    return user.privilegedHint || m_privileged.contains(user.name);
}

bool UploadQueue::goesBefore(const User &a, const User &b) const
{
    const bool privilegedA = isPrivileged(a);
    const bool privilegedB = isPrivileged(b);
    if (privilegedA != privilegedB) {
        return privilegedA;
    }
    if (a.turn != b.turn) {
        return a.turn < b.turn;
    }
    return a.arrival < b.arrival;
}

void UploadQueue::dropIfIdle(QHash<QString, User>::iterator it)
{
    if (it->queued.isEmpty() && it->active.isEmpty()) {
        m_users.erase(it);
    }
}

} // namespace selecta::share
