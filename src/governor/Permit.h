// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <utility>

namespace selecta::governor {

class Governor;

namespace testing {
class PermitFactory; // test code only; defined in tests/, never linked into the app
} // namespace testing

// Every outbound operation the network governor budgets (ADR-006, docs/RESEARCH-SYNTHESIS.md §3).
enum class Operation {
    // Soulseek server and peers
    Login,          // a server login attempt (reconnects included)
    Search,         // FileSearch, UserSearch, RoomSearch, WishlistSearch: one shared budget
    PeerLookup,     // GetPeerAddress / ConnectToPeer
    QueueDownload,  // QueueUpload sent to a peer (downloads and previews)
    PlaceInQueue,   // PlaceInQueueRequest
    Browse,         // GetSharedFileList
    UserInfo,       // UserInfoRequest
    FolderContents, // FolderContentsRequest
    PrivateMessage, // MessageUser
    RoomMessage,    // SayChatroom
    // Metadata and previews (meta/)
    SpotifyRequest,
    MusicBrainzRequest,
    CoverArtRequest,
    DeezerRequest,
    YtDlpRun,
};

// Proof that the governor allowed one operation. Only the Governor (and test code) can create one, so code that
// sends a budgeted message must ask the governor first: the send path is unreachable otherwise (CLAUDE.md network
// rules). A Permit is move-only and single use: the function that performs the operation takes it by value.
class Permit
{
public:
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;
    Permit(Permit &&other) noexcept
        : m_operation(other.m_operation)
        , m_valid(std::exchange(other.m_valid, false))
    {}
    Permit &operator=(Permit &&other) noexcept
    {
        m_operation = other.m_operation;
        m_valid = std::exchange(other.m_valid, false);
        return *this;
    }
    ~Permit() = default;

    Operation operation() const { return m_operation; }
    // False once moved from.
    bool isValid() const { return m_valid; }

    // Consumes the permit for `operation`. Returns false (and asserts in debug builds) if it was already used or was
    // issued for another operation.
    bool consume(Operation operation)
    {
        const bool ok = m_valid && m_operation == operation;
        Q_ASSERT_X(ok, "governor::Permit", "used twice, or for another operation than it was issued for");
        m_valid = false;
        return ok;
    }

private:
    friend class Governor;
    friend class testing::PermitFactory;

    explicit Permit(Operation operation)
        : m_operation(operation)
    {}

    Operation m_operation;
    bool m_valid = true;
};

} // namespace selecta::governor
