// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/Database.h"

#include <sqlite3.h>
#include <utility>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcStorage, "selecta.storage")

namespace selecta::storage {
namespace {

QString errorText(sqlite3 *db, QStringView what)
{
    const char *message = db ? sqlite3_errmsg(db) : "out of memory";
    return what.toString() + u": "_s + QString::fromUtf8(message);
}

// True when `tail` holds only whitespace and SQL comments.
bool isBlankSql(const char *tail)
{
    const char *p = tail;
    while (p && *p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';') {
            ++p;
        } else if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') {
                ++p;
            }
        } else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && (p[0] != '*' || p[1] != '/')) {
                ++p;
            }
            if (*p) {
                p += 2;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

Database::Database() = default;

Database::~Database()
{
    close();
}

void Database::assertOwnerThread() const
{
    Q_ASSERT_X(!m_db || m_owner == std::this_thread::get_id(), "selecta::storage::Database",
               "used from a thread other than the one that opened it");
}

bool Database::open(const QString &path)
{
    close();
    if (path.isEmpty()) {
        m_lastError = u"open: empty path (use \":memory:\" for an in-memory database)"_s;
        return false;
    }
    m_owner = std::this_thread::get_id();

    const QByteArray utf8Path = path.toUtf8();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(utf8Path.constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        recordError(u"open");
        close(); // sqlite3_open_v2 can return a handle even on failure
        return false;
    }
    sqlite3_busy_timeout(m_db, 5000);

    if (!exec(u"PRAGMA foreign_keys = ON;")) {
        close();
        return false;
    }
    if (path != u":memory:"_s) {
        if (!exec(u"PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;")) {
            close();
            return false;
        }
        // WAL can be refused (e.g. on some network file systems); SQLite then keeps the old mode without an error.
        if (const QString mode = journalMode(); mode != u"wal"_s) {
            qCWarning(lcStorage) << "WAL is unavailable for" << path << "- journal mode is" << mode;
        }
    }
    qCDebug(lcStorage) << "opened" << path;
    return true;
}

void Database::close()
{
    if (!m_db) {
        return;
    }
    assertOwnerThread();
    sqlite3_close_v2(m_db); // with unfinalized statements, the connection becomes a zombie until they are finalized
    m_db = nullptr;
}

bool Database::exec(QStringView sql)
{
    if (!m_db) {
        m_lastError = u"exec: database is not open"_s;
        return false;
    }
    assertOwnerThread();
    char *message = nullptr;
    const QByteArray utf8 = sql.toUtf8();
    if (sqlite3_exec(m_db, utf8.constData(), nullptr, nullptr, &message) != SQLITE_OK) {
        m_lastError = u"exec: "_s + QString::fromUtf8(message ? message : sqlite3_errmsg(m_db));
        sqlite3_free(message);
        qCWarning(lcStorage) << m_lastError;
        return false;
    }
    return true;
}

Statement Database::prepare(QStringView sql)
{
    if (!m_db) {
        m_lastError = u"prepare: database is not open"_s;
        return {};
    }
    assertOwnerThread();
    sqlite3_stmt *stmt = nullptr;
    const char *tail = nullptr;
    const QByteArray utf8 = sql.toUtf8();
    if (sqlite3_prepare_v2(m_db, utf8.constData(), static_cast<int>(utf8.size()) + 1, &stmt, &tail) != SQLITE_OK) {
        recordError(u"prepare");
        return {};
    }
    if (!stmt) {
        m_lastError = u"prepare: no statement in the SQL"_s;
        return {};
    }
    if (!isBlankSql(tail)) {
        sqlite3_finalize(stmt);
        m_lastError = u"prepare: more than one statement; use exec() for scripts"_s;
        qCWarning(lcStorage) << m_lastError;
        return {};
    }
    return Statement(stmt);
}

int Database::userVersion()
{
    Statement statement = prepare(u"PRAGMA user_version;");
    if (statement.isValid() && statement.step() == Statement::Step::Row) {
        return static_cast<int>(statement.int64(0));
    }
    return -1;
}

bool Database::setUserVersion(int version)
{
    return exec(u"PRAGMA user_version = %1;"_s.arg(version));
}

QString Database::journalMode()
{
    Statement statement = prepare(u"PRAGMA journal_mode;");
    if (statement.isValid() && statement.step() == Statement::Step::Row) {
        return statement.text(0).toLower();
    }
    return {};
}

qint64 Database::lastInsertRowId() const
{
    assertOwnerThread();
    return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
}

int Database::changes() const
{
    assertOwnerThread();
    return m_db ? sqlite3_changes(m_db) : 0;
}

void Database::recordError(QStringView what)
{
    m_lastError = errorText(m_db, what);
    qCWarning(lcStorage) << m_lastError;
}

// --- Statement ------------------------------------------------------------------------------------------------------

Statement::~Statement()
{
    if (m_stmt) {
        sqlite3_finalize(m_stmt);
    }
}

Statement::Statement(Statement &&other) noexcept
    : m_stmt(std::exchange(other.m_stmt, nullptr))
    , m_hasRow(std::exchange(other.m_hasRow, false))
    , m_bindFailed(std::exchange(other.m_bindFailed, false))
    , m_lastError(std::move(other.m_lastError))
{}

Statement &Statement::operator=(Statement &&other) noexcept
{
    if (this != &other) {
        if (m_stmt) {
            sqlite3_finalize(m_stmt);
        }
        m_stmt = std::exchange(other.m_stmt, nullptr);
        m_hasRow = std::exchange(other.m_hasRow, false);
        m_bindFailed = std::exchange(other.m_bindFailed, false);
        m_lastError = std::move(other.m_lastError);
    }
    return *this;
}

bool Statement::checkBind(int resultCode, int index)
{
    if (resultCode == SQLITE_OK) {
        return true;
    }
    m_bindFailed = true;
    m_lastError = u"bind parameter %1: "_s.arg(index) + QString::fromUtf8(sqlite3_errstr(resultCode));
    qCWarning(lcStorage) << m_lastError;
    return false;
}

Statement &Statement::bind(int index, qint64 value)
{
    if (m_stmt) {
        checkBind(sqlite3_bind_int64(m_stmt, index, value), index);
    }
    return *this;
}

Statement &Statement::bind(int index, double value)
{
    if (m_stmt) {
        checkBind(sqlite3_bind_double(m_stmt, index, value), index);
    }
    return *this;
}

Statement &Statement::bind(int index, const QString &value)
{
    if (m_stmt) {
        const QByteArray utf8 = value.toUtf8();
        checkBind(sqlite3_bind_text64(m_stmt, index, utf8.constData(), static_cast<sqlite3_uint64>(utf8.size()),
                                      SQLITE_TRANSIENT, SQLITE_UTF8),
                  index);
    }
    return *this;
}

Statement &Statement::bind(int index, const QByteArray &value)
{
    if (m_stmt) {
        // A null QByteArray binds NULL; an empty one binds a zero-length blob.
        const int rc = value.isNull()
                           ? sqlite3_bind_null(m_stmt, index)
                           : sqlite3_bind_blob64(m_stmt, index, value.constData(),
                                                 static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT);
        checkBind(rc, index);
    }
    return *this;
}

Statement &Statement::bindNull(int index)
{
    if (m_stmt) {
        checkBind(sqlite3_bind_null(m_stmt, index), index);
    }
    return *this;
}

Statement::Step Statement::step()
{
    m_hasRow = false;
    if (!m_stmt) {
        m_lastError = u"step: invalid statement"_s;
        return Step::Error;
    }
    if (m_bindFailed) {
        return Step::Error;
    }
    switch (const int rc = sqlite3_step(m_stmt)) {
    case SQLITE_ROW:
        m_hasRow = true;
        return Step::Row;
    case SQLITE_DONE:
        return Step::Done;
    default:
        m_lastError = errorText(sqlite3_db_handle(m_stmt), u"step");
        qCWarning(lcStorage) << m_lastError << "(code" << rc << ')';
        return Step::Error;
    }
}

bool Statement::run()
{
    Step result = step();
    while (result == Step::Row) {
        result = step();
    }
    return result == Step::Done;
}

void Statement::reset()
{
    if (!m_stmt) {
        return;
    }
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
    m_hasRow = false;
    m_bindFailed = false;
    m_lastError.clear();
}

int Statement::columnCount() const
{
    return m_stmt ? sqlite3_column_count(m_stmt) : 0;
}

bool Statement::hasColumn(int column) const
{
    return m_stmt && m_hasRow && column >= 0 && column < sqlite3_column_count(m_stmt);
}

qint64 Statement::int64(int column) const
{
    return hasColumn(column) ? sqlite3_column_int64(m_stmt, column) : 0;
}

double Statement::real(int column) const
{
    return hasColumn(column) ? sqlite3_column_double(m_stmt, column) : 0.0;
}

QString Statement::text(int column) const
{
    if (!hasColumn(column)) {
        return {};
    }
    // Call sqlite3_column_text before sqlite3_column_bytes: the order matters when a conversion happens.
    const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, column));
    return QString::fromUtf8(data, sqlite3_column_bytes(m_stmt, column));
}

QByteArray Statement::blob(int column) const
{
    if (!hasColumn(column)) {
        return {};
    }
    const auto *data = static_cast<const char *>(sqlite3_column_blob(m_stmt, column));
    return {data, sqlite3_column_bytes(m_stmt, column)};
}

bool Statement::isNull(int column) const
{
    return !hasColumn(column) || sqlite3_column_type(m_stmt, column) == SQLITE_NULL;
}

// --- Transaction ----------------------------------------------------------------------------------------------------

Transaction::Transaction(Database &db)
    : m_db(db)
    , m_active(db.exec(u"BEGIN IMMEDIATE;"))
{}

Transaction::~Transaction()
{
    if (m_active && !m_db.exec(u"ROLLBACK;")) {
        qCWarning(lcStorage) << "rollback failed:" << m_db.lastError();
    }
}

bool Transaction::commit()
{
    if (!m_active) {
        return false;
    }
    if (!m_db.exec(u"COMMIT;")) {
        return false; // still active: the destructor rolls back
    }
    m_active = false;
    return true;
}

} // namespace selecta::storage
