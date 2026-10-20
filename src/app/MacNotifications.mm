// SPDX-License-Identifier: GPL-3.0-or-later
// System notifications on macOS (PRD UI-6, NET-7): UNUserNotificationCenter, from inside the app bundle only.
// Compiled with ARC.
#include "app/Notifications.h"

#include <QLoggingCategory>
#include <QString>

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>
#include <cstddef>
#include <deque>

using namespace Qt::StringLiterals;

namespace selecta::app {
class MacNotificationSink;
} // namespace selecta::app

// Hears from Notification Center and hands what matters to the sink, on the main thread. It holds no strong reference
// back: the sink clears `owner` before it goes.
@interface SelectaNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, assign) selecta::app::MacNotificationSink *owner; // main thread only
@end

namespace {

Q_LOGGING_CATEGORY(lcNotifications, "selecta.app.notifications")

NSString *const kViewKey = @"selecta.view";
NSString *const kArgumentKey = @"selecta.argument";
// What a click hands back comes from the system's store: anything longer isn't Selecta's.
constexpr qsizetype kMaxTargetLength = 512;

// Runs `work` later on the main thread, where Qt and the sink live.
void onMainThread(dispatch_block_t work)
{
    dispatch_async(dispatch_get_main_queue(), work);
}

QString targetPart(NSDictionary *info, NSString *key)
{
    id const value = info[key];
    if (![value isKindOfClass:[NSString class]]) {
        return {};
    }
    NSString *const text = value;
    const QString part = QString::fromNSString(text);
    return part.size() <= kMaxTargetLength ? part : QString();
}

} // namespace

namespace selecta::app {

// Posts to macOS's Notification Center. Permission is asked the first time there is something to show, never at
// launch. If the user refuses, nothing more happens: Selecta doesn't ask again (macOS wouldn't show the question twice
// anyway), what would have been shown is dropped, and the window's own notifications and the Dock badge carry on as
// before. Allowing them later in System Settings → Notifications → Selecta works from the next notification on: each
// one checks the permission first.
//
// Main thread only. The system answers on its own queues; the answers come back through the main queue and find the
// sink through the delegate's `owner`, which the destructor clears.
class MacNotificationSink final : public NotificationSink
{
public:
    MacNotificationSink();
    ~MacNotificationSink() override;
    MacNotificationSink(const MacNotificationSink &) = delete;
    MacNotificationSink &operator=(const MacNotificationSink &) = delete;
    MacNotificationSink(MacNotificationSink &&) = delete;
    MacNotificationSink &operator=(MacNotificationSink &&) = delete;

    void post(const SystemNotification &notification) override;
    QString describe() const override;

    // Called on the main thread, from the delegate and the system's answers.
    void clicked(const NotificationTarget &target) const { activate(target); }
    void permissionKnown(UNAuthorizationStatus status);
    void permissionAnswered(bool granted, const QString &problem);
    void deliveryFailed(const QString &problem);

private:
    enum class Permission { Unknown, Allowed, Refused };
    static constexpr std::size_t kMaxWaiting = 20;

    void checkPermission();
    void deliverWaiting();
    void deliver(const SystemNotification &notification);

    SelectaNotificationDelegate *m_delegate;
    std::deque<SystemNotification> m_waiting; // posted while the permission is checked or asked
    Permission m_permission = Permission::Unknown;
    bool m_checking = false; // a check of the permission is under way
    bool m_asking = false;   // the user is being asked
    bool m_asked = false;    // the user was asked during this run
    QString m_problem;       // the last thing that went wrong, for describe()
};

MacNotificationSink::MacNotificationSink()
    : m_delegate([[SelectaNotificationDelegate alloc] init])
{
    m_delegate.owner = this;
    @autoreleasepool {
        // Set before the event loop starts, so a click that launches Selecta isn't missed.
        UNUserNotificationCenter.currentNotificationCenter.delegate = m_delegate;
    }
}

MacNotificationSink::~MacNotificationSink()
{
    m_delegate.owner = nullptr; // answers still on their way find nobody
    @autoreleasepool {
        UNUserNotificationCenter *const center = UNUserNotificationCenter.currentNotificationCenter;
        if (center.delegate == m_delegate) {
            center.delegate = nil;
        }
    }
}

void MacNotificationSink::post(const SystemNotification &notification)
{
    m_waiting.push_back(notification);
    while (m_waiting.size() > kMaxWaiting) {
        m_waiting.pop_front();
    }
    if (!m_checking && !m_asking) {
        checkPermission(); // every time: the user may have changed it in System Settings
    }
}

QString MacNotificationSink::describe() const
{
    QString text;
    switch (m_permission) {
    case Permission::Unknown:
        text =
            m_asking ? u"macOS Notification Center: asking the user"_s : u"macOS Notification Center: not asked yet"_s;
        break;
    case Permission::Allowed:
        text = u"macOS Notification Center: allowed"_s;
        break;
    case Permission::Refused:
        text = u"macOS Notification Center: refused (System Settings → Notifications → Selecta)"_s;
        break;
    }
    return m_problem.isEmpty() ? text : text + u"; last problem: "_s + m_problem;
}

void MacNotificationSink::checkPermission()
{
    m_checking = true;
    SelectaNotificationDelegate *const delegate = m_delegate;
    @autoreleasepool {
        [UNUserNotificationCenter.currentNotificationCenter
            getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
                const UNAuthorizationStatus status = settings.authorizationStatus;
                onMainThread(^{
                    if (MacNotificationSink *owner = delegate.owner) {
                        owner->permissionKnown(status);
                    }
                });
            }];
    }
}

void MacNotificationSink::permissionKnown(UNAuthorizationStatus status)
{
    m_checking = false;
    if (status == UNAuthorizationStatusAuthorized || status == UNAuthorizationStatusProvisional) {
        m_permission = Permission::Allowed;
        deliverWaiting();
        return;
    }
    if (status == UNAuthorizationStatusDenied) {
        if (m_permission != Permission::Refused) {
            qCInfo(lcNotifications) << "notifications are turned off for Selecta in System Settings";
        }
        m_permission = Permission::Refused;
        m_waiting.clear();
        return;
    }
    // Not decided yet: ask, once per run. A user who closes the question without answering isn't asked again until
    // the next launch.
    if (m_asked) {
        m_waiting.clear();
        return;
    }
    m_asked = true;
    m_asking = true;
    SelectaNotificationDelegate *const delegate = m_delegate;
    @autoreleasepool {
        [UNUserNotificationCenter.currentNotificationCenter
            requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError *error) {
                              NSString *const problem = error.localizedDescription;
                              onMainThread(^{
                                  if (MacNotificationSink *owner = delegate.owner) {
                                      owner->permissionAnswered(granted == YES,
                                                                problem ? QString::fromNSString(problem) : QString());
                                  }
                              });
                          }];
    }
}

void MacNotificationSink::permissionAnswered(bool granted, const QString &problem)
{
    m_asking = false;
    if (!problem.isEmpty()) {
        m_problem = problem;
        qCWarning(lcNotifications).noquote() << "asking for permission to notify failed:" << problem;
    }
    if (!granted) {
        // Refused: drop what waited; the window's notifications and the Dock badge carry on (see the class comment).
        qCInfo(lcNotifications) << "the user didn't allow notifications";
        m_permission = Permission::Refused;
        m_waiting.clear();
        return;
    }
    m_permission = Permission::Allowed;
    deliverWaiting();
}

void MacNotificationSink::deliveryFailed(const QString &problem)
{
    m_problem = problem;
    qCWarning(lcNotifications).noquote() << "a notification couldn't be shown:" << problem;
}

void MacNotificationSink::deliverWaiting()
{
    while (!m_waiting.empty()) {
        const SystemNotification notification = m_waiting.front();
        m_waiting.pop_front();
        deliver(notification);
    }
}

void MacNotificationSink::deliver(const SystemNotification &notification)
{
    SelectaNotificationDelegate *const delegate = m_delegate;
    @autoreleasepool {
        UNMutableNotificationContent *const content = [[UNMutableNotificationContent alloc] init];
        content.title = notification.title.toNSString();
        content.body = notification.body.toNSString();
        if (!notification.thread.isEmpty()) {
            content.threadIdentifier = notification.thread.toNSString();
        }
        content.userInfo = @{
            kViewKey : notification.target.view.toNSString(),
            kArgumentKey : notification.target.argument.toNSString()
        };
        if (notification.sound) {
            content.sound = UNNotificationSound.defaultSound;
        }
        NSString *const identifier = notification.id.isEmpty() ? NSUUID.UUID.UUIDString : notification.id.toNSString();
        UNNotificationRequest *const request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                    content:content
                                                                                    trigger:nil];
        [UNUserNotificationCenter.currentNotificationCenter
            addNotificationRequest:request
             withCompletionHandler:^(NSError *error) {
                 if (error == nil) {
                     return;
                 }
                 NSString *const problem = error.localizedDescription;
                 onMainThread(^{
                     if (MacNotificationSink *owner = delegate.owner) {
                         owner->deliveryFailed(QString::fromNSString(problem));
                     }
                 });
             }];
    }
}

bool runsInAppBundle()
{
    @autoreleasepool {
        NSBundle *const bundle = NSBundle.mainBundle;
        return bundle.bundleIdentifier.length > 0 && [bundle.bundleURL.pathExtension isEqualToString:@"app"];
    }
}

std::unique_ptr<NotificationSink> createSystemNotificationSink()
{
    if (!runsInAppBundle()) {
        qCInfo(lcNotifications) << "not running from an app bundle: no system notifications";
        return nullptr; // UNUserNotificationCenter would throw
    }
    @try {
        @autoreleasepool {
            (void)UNUserNotificationCenter.currentNotificationCenter;
        }
    } @catch (NSException *exception) {
        qCWarning(lcNotifications).noquote()
            << "macOS's notifications aren't available:" << QString::fromNSString(exception.reason);
        return nullptr;
    }
    return std::make_unique<MacNotificationSink>();
}

} // namespace selecta::app

@implementation SelectaNotificationDelegate

// A click on one of Selecta's notifications: Selecta comes forward and shows what it was about.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)())completionHandler
{
    (void)center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        NSDictionary *const info = response.notification.request.content.userInfo;
        onMainThread(^{
            selecta::app::MacNotificationSink *owner = self.owner;
            if (owner == nullptr) {
                return;
            }
            [NSApp activate];
            owner->clicked({targetPart(info, kViewKey), targetPart(info, kArgumentKey)});
        });
    }
    completionHandler();
}

// Delivered while Selecta is the active app (it came forward after posting): the window's own notification is enough.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionNone);
}

@end
