// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/EngineHost.h"

#include "governor/Limits.h"

#include <QLoggingCategory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcEngineHost, "selecta.core.enginehost")

} // namespace

EngineHost::EngineHost(EngineOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_paused(m_options.startPaused)
    , m_searchesAvailable(governor::limits::kSearchBurst)
{
    qRegisterMetaType<ActivityEntry>();
    m_thread.setObjectName(u"selecta-engine"_s);
}

EngineHost::~EngineHost()
{
    stop();
}

void EngineHost::start()
{
    if (m_thread.isRunning()) {
        return;
    }
    auto *engine = new Engine(m_options); // deleted on its own thread when the thread finishes
    engine->moveToThread(&m_thread);
    m_engine = engine;
    connect(&m_thread, &QThread::finished, engine, &QObject::deleteLater);

    // Engine → UI: queued across the threads, so these run on the UI thread.
    connect(engine, &Engine::startFinished, this, [this](bool ok, const QString &error) {
        if (!ok) {
            qCCritical(lcEngineHost).noquote() << "engine didn't start:" << error;
        }
        Q_EMIT started(ok, error);
    });
    connect(engine, &Engine::pausedChanged, this, [this](bool paused) {
        m_paused = paused;
        Q_EMIT pausedChanged(paused);
    });
    connect(engine, &Engine::searchBudgetChanged, this, [this](int available) {
        m_searchesAvailable = available;
        Q_EMIT searchBudgetChanged(available);
    });
    connect(engine, &Engine::activityRecorded, this, &EngineHost::activityRecorded);

    m_thread.start();
    QMetaObject::invokeMethod(engine, &Engine::start, Qt::QueuedConnection);
}

void EngineHost::stop()
{
    if (!m_thread.isRunning()) {
        return;
    }
    if (m_engine) {
        // Waits for the engine to close its services and database on its own thread.
        QMetaObject::invokeMethod(m_engine.data(), &Engine::stop, Qt::BlockingQueuedConnection);
    }
    m_thread.quit();
    m_thread.wait();
}

void EngineHost::setPaused(bool paused)
{
    post([paused](Engine &engine) { engine.setPaused(paused); });
}

} // namespace selecta::core
