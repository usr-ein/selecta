// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QLoggingCategory>
#include <QString>
#include <QStringView>

#include <thread>

struct sqlite3;
struct sqlite3_stmt;

Q_DECLARE_LOGGING_CATEGORY(lcStorage)

namespace selecta::storage {

class Statement;

// One SQLite connection (ADR-005).
//
// Threading: a connection belongs to the thread that opened it and must only be used there (SQLite is built with
// SQLITE_THREADSAFE=2). Debug builds assert this. Other threads open their own connection; WAL lets readers and one
// writer work concurrently.
//
// Errors are reported through return values and lastError(); nothing throws. A Database can't be copied or moved, so
// Statements and Transactions never outlive the object they came from by accident.
class Database
{
public:
    Database();
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;

    // Opens or creates the database at `path`, or an in-memory one for ":memory:". File databases use WAL; every
    // connection enables foreign keys and a 5 s busy timeout. An already open connection is closed first.
    [[nodiscard]] bool open(const QString &path);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // Runs one or more statements that return no rows.
    [[nodiscard]] bool exec(QStringView sql);

    // Compiles exactly one statement; trailing SQL other than whitespace or comments is an error. Check
    // Statement::isValid(). The statement may outlive close(): SQLite frees the connection once it is finalized.
    [[nodiscard]] Statement prepare(QStringView sql);

    // PRAGMA user_version, or -1 if it can't be read.
    int userVersion();
    [[nodiscard]] bool setUserVersion(int version);
    QString journalMode();

    qint64 lastInsertRowId() const;
    int changes() const;
    QString lastError() const { return m_lastError; }

private:
    void assertOwnerThread() const;
    void recordError(QStringView what);

    sqlite3 *m_db = nullptr;
    QString m_lastError;
    std::thread::id m_owner;
};

// A compiled statement. Bind parameters (1-based), then step(); read columns (0-based) while step() returns Row.
// Binding errors are remembered and make step() fail, so a bad bind can't silently run with NULLs.
class Statement
{
public:
    enum class Step { Row, Done, Error };

    Statement() = default;
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    Statement(Statement &&other) noexcept;
    Statement &operator=(Statement &&other) noexcept;

    bool isValid() const { return m_stmt != nullptr; }

    Statement &bind(int index, qint64 value);
    Statement &bind(int index, int value) { return bind(index, qint64{value}); }
    Statement &bind(int index, double value);
    Statement &bind(int index, const QString &value);
    Statement &bind(int index, const QByteArray &value);
    Statement &bindNull(int index);

    Step step();
    // Steps to completion, ignoring rows. Returns false on any error.
    [[nodiscard]] bool run();
    // Rewinds the statement and clears its bindings and errors, ready to bind again.
    void reset();

    // Column readers return a null/zero value when there is no current row or the column doesn't exist.
    int columnCount() const;
    qint64 int64(int column) const;
    double real(int column) const;
    QString text(int column) const;
    QByteArray blob(int column) const;
    bool isNull(int column) const;

    QString lastError() const { return m_lastError; }

private:
    friend class Database;
    explicit Statement(sqlite3_stmt *stmt)
        : m_stmt(stmt)
    {}
    bool checkBind(int resultCode, int index);
    bool hasColumn(int column) const;

    sqlite3_stmt *m_stmt = nullptr;
    bool m_hasRow = false;
    bool m_bindFailed = false;
    QString m_lastError;
};

// A write transaction (BEGIN IMMEDIATE). Rolls back on destruction unless commit() succeeded. Not nestable.
class Transaction
{
public:
    explicit Transaction(Database &db);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    bool isActive() const { return m_active; }
    [[nodiscard]] bool commit();

private:
    Database &m_db;
    bool m_active = false;
};

} // namespace selecta::storage
