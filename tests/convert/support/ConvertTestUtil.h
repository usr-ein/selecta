// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QByteArray>
#include <QColor>
#include <QString>

// Helpers for the convert/ tests: files on disk, synthetic covers, a rich metadata model.
namespace selecta::testing {

// Writes `bytes` to `directory`/`name` and returns the path (empty on failure).
QString writeFile(const QString &directory, const QString &name, const QByteArray &bytes);
QByteArray readFile(const QString &path);

// A synthetic image (gradient with a mark in one corner), encoded as `format` ("jpeg", "png").
QByteArray makeImage(int width, int height, const char *format, int quality = 90);

// A model that exercises every mapping: several artists, non-Latin text, totals, decimals in BPM, Camelot key, label,
// ISRC, a source link, v2.4-only fields, a custom field and a JPEG front cover.
convert::MetadataModel richModel();

} // namespace selecta::testing
