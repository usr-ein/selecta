// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"

#include <QByteArray>
#include <QJsonObject>

#include <optional>

namespace selecta::core {

// meta::Release ↔ JSON, for the database (Get tracks lines, Inbox items: the release a file was tagged from). Every
// field round-trips; reading is defensive (stored text is ours, but a damaged database must not crash anything):
// unknown keys are ignored, wrong types read as empty.
QJsonObject releaseToJson(const meta::Release &release);
meta::Release releaseFromJson(const QJsonObject &object);

// Compact JSON text, and back. An empty or unreadable text gives nothing.
QByteArray releaseToText(const meta::Release &release);
std::optional<meta::Release> releaseFromText(const QByteArray &text);

} // namespace selecta::core
