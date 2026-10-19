// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/Database.h"

#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::storage::Database;
using selecta::storage::Statement;
using selecta::storage::Transaction;

namespace {

class DatabaseTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s)) << db.lastError().toStdString();
        ASSERT_TRUE(db.exec(u"CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, data BLOB, x REAL) STRICT;"));
    }

    int rowCount()
    {
        Statement count = db.prepare(u"SELECT COUNT(*) FROM t;");
        return count.step() == Statement::Step::Row ? static_cast<int>(count.int64(0)) : -1;
    }

    Database db;
};

} // namespace

TEST_F(DatabaseTest, RoundTripsValues)
{
    const QString name = u"Inés — Ωmega 🎧"_s;
    const QByteArray data("\x00\x01\xff zero inside", 15);
    Statement insert = db.prepare(u"INSERT INTO t (name, data, x) VALUES (?1, ?2, ?3);");
    ASSERT_TRUE(insert.isValid());
    ASSERT_TRUE(insert.bind(1, name).bind(2, data).bind(3, 1.25).run());
    EXPECT_EQ(db.lastInsertRowId(), 1);
    EXPECT_EQ(db.changes(), 1);

    Statement select = db.prepare(u"SELECT id, name, data, x FROM t;");
    ASSERT_EQ(select.step(), Statement::Step::Row);
    EXPECT_EQ(select.int64(0), 1);
    EXPECT_EQ(select.text(1), name);
    EXPECT_EQ(select.blob(2), data);
    EXPECT_DOUBLE_EQ(select.real(3), 1.25);
    EXPECT_EQ(select.step(), Statement::Step::Done);
}

TEST_F(DatabaseTest, NullAndEmptyAreDistinct)
{
    Statement insert = db.prepare(u"INSERT INTO t (name, data) VALUES (?1, ?2);");
    ASSERT_TRUE(insert.bindNull(1).bind(2, QByteArray()).run()); // null QByteArray binds NULL
    insert.reset();
    ASSERT_TRUE(insert.bind(1, QString(u""_s)).bind(2, QByteArray("")).run());

    Statement select = db.prepare(u"SELECT name IS NULL, data IS NULL FROM t ORDER BY id;");
    ASSERT_EQ(select.step(), Statement::Step::Row);
    EXPECT_EQ(select.int64(0), 1);
    EXPECT_EQ(select.int64(1), 1);
    ASSERT_EQ(select.step(), Statement::Step::Row);
    EXPECT_EQ(select.int64(0), 0);
    EXPECT_EQ(select.int64(1), 0);
}

TEST_F(DatabaseTest, PrepareRefusesSeveralStatements)
{
    EXPECT_FALSE(db.prepare(u"SELECT 1; SELECT 2;").isValid());
    EXPECT_TRUE(db.lastError().contains(u"more than one statement"_s));
    EXPECT_TRUE(db.prepare(u"SELECT 1; -- trailing comment\n  /* and another */ ;").isValid());
}

TEST_F(DatabaseTest, PrepareRefusesEmptyAndInvalidSql)
{
    EXPECT_FALSE(db.prepare(u"   ").isValid());
    EXPECT_FALSE(db.prepare(u"SELEC nonsense").isValid());
    EXPECT_FALSE(db.lastError().isEmpty());
}

TEST_F(DatabaseTest, BadBindMakesStepFail)
{
    Statement insert = db.prepare(u"INSERT INTO t (name) VALUES (?1);");
    insert.bind(2, u"out of range"_s);
    EXPECT_EQ(insert.step(), Statement::Step::Error);
    EXPECT_FALSE(insert.lastError().isEmpty());
    EXPECT_EQ(rowCount(), 0);

    insert.reset(); // clears the error
    EXPECT_TRUE(insert.bind(1, u"ok"_s).run());
    EXPECT_EQ(rowCount(), 1);
}

TEST_F(DatabaseTest, ColumnsWithoutARowReadAsDefaults)
{
    Statement select = db.prepare(u"SELECT name FROM t;");
    EXPECT_EQ(select.int64(0), 0);
    EXPECT_TRUE(select.text(0).isEmpty());
    EXPECT_TRUE(select.isNull(0));
    EXPECT_EQ(select.step(), Statement::Step::Done); // empty table
    EXPECT_TRUE(select.isNull(5));
}

TEST_F(DatabaseTest, ConstraintViolationIsAnError)
{
    ASSERT_TRUE(db.exec(u"INSERT INTO t (id) VALUES (1);"));
    Statement duplicate = db.prepare(u"INSERT INTO t (id) VALUES (1);");
    EXPECT_EQ(duplicate.step(), Statement::Step::Error);
    EXPECT_TRUE(duplicate.lastError().contains(u"UNIQUE"_s, Qt::CaseInsensitive));
}

TEST_F(DatabaseTest, TransactionRollsBackUnlessCommitted)
{
    {
        Transaction transaction(db);
        ASSERT_TRUE(transaction.isActive());
        ASSERT_TRUE(db.exec(u"INSERT INTO t (name) VALUES ('rolled back');"));
    }
    EXPECT_EQ(rowCount(), 0);
    {
        Transaction transaction(db);
        ASSERT_TRUE(db.exec(u"INSERT INTO t (name) VALUES ('kept');"));
        ASSERT_TRUE(transaction.commit());
        EXPECT_FALSE(transaction.commit()); // already committed
    }
    EXPECT_EQ(rowCount(), 1);
}

TEST_F(DatabaseTest, TransactionsDoNotNest)
{
    Transaction outer(db);
    ASSERT_TRUE(outer.isActive());
    Transaction inner(db);
    EXPECT_FALSE(inner.isActive());
}

TEST_F(DatabaseTest, StatementMayOutliveTheConnection)
{
    Statement select = db.prepare(u"SELECT 1;");
    ASSERT_TRUE(select.isValid());
    db.close(); // the connection stays alive as a zombie until the statement is finalized
    EXPECT_FALSE(db.isOpen());
}

TEST(Database, OperationsOnAClosedDatabaseFail)
{
    Database db;
    EXPECT_FALSE(db.exec(u"SELECT 1;"));
    EXPECT_FALSE(db.prepare(u"SELECT 1;").isValid());
    EXPECT_EQ(db.userVersion(), -1);
    EXPECT_FALSE(db.lastError().isEmpty());
}

TEST(Database, EmptyPathIsRefused)
{
    Database db;
    EXPECT_FALSE(db.open(QString()));
    EXPECT_FALSE(db.isOpen());
}

TEST(Database, FileDatabaseUsesWalAndPersists)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(u"test.sqlite3"_s);
    {
        Database db;
        ASSERT_TRUE(db.open(path));
        EXPECT_EQ(db.journalMode(), u"wal"_s);
        ASSERT_TRUE(db.exec(u"CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT) STRICT; INSERT INTO kv VALUES ('a', 'b');"));
        ASSERT_TRUE(db.setUserVersion(7));
    }
    Database db;
    ASSERT_TRUE(db.open(path));
    EXPECT_EQ(db.userVersion(), 7);
    Statement select = db.prepare(u"SELECT v FROM kv WHERE k = ?1;");
    ASSERT_EQ(select.bind(1, u"a"_s).step(), Statement::Step::Row);
    EXPECT_EQ(select.text(0), u"b"_s);
}

TEST(Database, ForeignKeysAreEnforced)
{
    Database db;
    ASSERT_TRUE(db.open(u":memory:"_s));
    ASSERT_TRUE(db.exec(u"CREATE TABLE parent (id INTEGER PRIMARY KEY) STRICT;"
                        u"CREATE TABLE child (parent INTEGER NOT NULL REFERENCES parent(id)) STRICT;"));
    EXPECT_FALSE(db.exec(u"INSERT INTO child VALUES (42);"));
}

TEST(Database, OpeningANonexistentFolderFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Database db;
    EXPECT_FALSE(db.open(dir.filePath(u"missing/folder/db.sqlite3"_s)));
    EXPECT_FALSE(db.isOpen());
    EXPECT_FALSE(db.lastError().isEmpty());
}
