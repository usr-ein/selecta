// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxRetag.h"

#include "convert/ArtworkProcessor.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"
#include "core/ArrivalService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

namespace keys = convert::keys;

QString translated(const char *text)
{
    return QCoreApplication::translate("selecta::core::InboxRetag", text);
}

RetagResult failure(const QString &error)
{
    RetagResult result;
    result.error = error;
    return result;
}

} // namespace

RetagResult retagInboxFile(const RetagRequest &request, const library::FileOps &fileOps)
{
    const QFileInfo info(request.path);
    if (!library::FileOps::isInside(request.path, request.inboxRoot) || !info.isFile()) {
        return failure(translated("The file isn't in the Inbox any more."));
    }
    convert::Result<convert::MetadataModel> own = convert::readTags(request.path);
    if (!own) {
        return failure(translated("Selecta can't read its tags: %1").arg(own.error().message));
    }
    convert::MetadataModel tags = own.takeValue();
    for (const QString &key : request.cleared) {
        tags.remove(key);
    }
    convert::MetadataModel fields = request.changes;
    fields.clearPictures();
    tags.overrideFrom(fields);
    if (const std::optional<convert::Picture> cover = request.changes.frontCover()) {
        if (convert::Result<convert::ProcessedArtwork> artwork = convert::processArtwork(cover->data); artwork) {
            tags.setFrontCover(artwork.value().toPicture(cover->provenance));
        } else {
            return failure(translated("The cover can't be used: %1").arg(artwork.error().message));
        }
    }

    // A copy next to the file takes the tags and is checked before it replaces the file.
    const QString copy = info.dir().filePath(u"."_s + info.fileName() + u".selecta-part"_s);
    const auto dropCopy = [&copy] { QFile::remove(copy); };
    dropCopy();
    if (!QFile::copy(request.path, copy)) {
        return failure(translated("Selecta can't write next to the file (is the disk full?)."));
    }
    if (const convert::Result<void> written = convert::writeTags(copy, tags); !written) {
        dropCopy();
        return failure(translated("The tags couldn't be written: %1").arg(written.error().message));
    }
    const convert::Result<convert::MetadataModel> check = convert::readTags(copy);
    if (!check || check.value().value(keys::Title) != tags.value(keys::Title) ||
        check.value().frontCover().has_value() != tags.frontCover().has_value()) {
        dropCopy();
        return failure(translated("The tags didn't read back as written; the file was left as it was."));
    }
    if (const library::FileResult swapped = fileOps.replaceInInbox(copy, request.path); !swapped.ok) {
        dropCopy();
        return failure(swapped.error);
    }

    // Named after its new tags (DL-13), in the same folder; a name already taken gets " (2)".
    QString path = request.path;
    const QString name = library::naming::componentName(request.templates.single, namingFieldsOf(tags), request.cleaner,
                                                        info.suffix().toLower());
    if (!name.isEmpty() && name != info.fileName()) {
        const QString inboxRoot = QFileInfo(request.inboxRoot).canonicalFilePath();
        const QString folder = QFileInfo(info.canonicalFilePath()).absolutePath();
        const QString relative = QDir(inboxRoot).relativeFilePath(QDir(folder).filePath(name));
        if (const library::FileResult renamed = fileOps.publishIntoInbox(info.canonicalFilePath(), relative);
            renamed.ok) {
            path = renamed.path;
        } // the tags are saved either way; a name that can't change stays as it was
    }

    RetagResult result;
    result.ok = true;
    result.path = path;
    result.tags = std::move(tags);
    result.sizeBytes = QFileInfo(path).size();
    return result;
}

} // namespace selecta::core
