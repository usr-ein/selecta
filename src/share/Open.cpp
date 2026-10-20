// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/Open.h"

#include "share/detail/Descriptor.h"

#include <QFile>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace selecta::share {

Opened open(const SharedFile &file)
{
    Opened opened;
    // The walk's bytes, unnormalised: the path opens on volumes that keep NFC names as well as on APFS.
    const QByteArray nativePath = file.localPath.toUtf8();
    const detail::CheckedDescriptor descriptor =
        detail::openChecked(nativePath, detail::FileIdentity{file.device, file.inode, file.size});
    if (descriptor.fd < 0) {
        switch (descriptor.failure) {
        case detail::OpenFailure::Gone:
        case detail::OpenFailure::Replaced:
        case detail::OpenFailure::Changed:
            opened.error = slsk::rejectionText(slsk::RejectionReason::FileNotShared);
            break;
        case detail::OpenFailure::None:
        case detail::OpenFailure::NotOnDisk:
        case detail::OpenFailure::Unreadable:
            opened.error = slsk::rejectionText(slsk::RejectionReason::FileReadError);
            break;
        }
        opened.detail = detail::describe(descriptor.failure, descriptor.error);
        return opened;
    }
    auto device = std::make_unique<QFile>();
    if (!device->open(descriptor.fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        (void)::close(descriptor.fd); // QFile doesn't take the descriptor when it refuses it
        opened.error = slsk::rejectionText(slsk::RejectionReason::FileReadError);
        opened.detail = u"it can't be opened as a file: %1"_s.arg(device->errorString());
        return opened;
    }
    opened.device = std::move(device);
    return opened;
}

} // namespace selecta::share
