// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Notifications.h"

#include <QList>

namespace selecta::testing {

// Stands in for the system's notification centre: keeps what is posted, and clicks on request. Tests never reach the
// real one.
class FakeNotificationSink final : public app::NotificationSink
{
public:
    void post(const app::SystemNotification &notification) override { posted.append(notification); }
    QString describe() const override { return QStringLiteral("fake"); }
    void click(const app::NotificationTarget &target) const { activate(target); }

    QList<app::SystemNotification> posted;
};

} // namespace selecta::testing
