#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

namespace relational_db {
namespace {

class TemporaryDatabase {
   public:
    TemporaryDatabase() {
        static std::atomic<uint64_t> next_id{0};
        path_ = std::filesystem::temp_directory_path() /
                ("custom-db-sql-execution-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(next_id.fetch_add(1)));
    }

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-wal.tmp", error);
        std::filesystem::remove(path_.string() + ".ack", error);
    }

    std::string Path() const {
        return path_.string();
    }

   private:
    std::filesystem::path path_;
};

struct SqlFixture {
    explicit SqlFixture(const std::string& path)
        : disk(std::make_shared<DiskManager>(path)), wal(path) {
        wal.Recover(*disk);
        bpm = std::make_shared<BufferPoolManager>(64, disk);
        executor = std::make_unique<Executor>(bpm, wal);
    }

    std::shared_ptr<DiskManager> disk;
    WriteAheadLog wal;
    std::shared_ptr<BufferPoolManager> bpm;
    std::unique_ptr<Executor> executor;
};

TEST(SqlExecutionTest, CreatesTablesAndExecutesIndexedSelects) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());

    EXPECT_NE(fixture.executor->ExecuteQuery(
                  "CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(64), age INT)"),
              "");
    fixture.executor->ExecuteQuery(
        "INSERT INTO people VALUES (1, 'Ada', 36), (2, 'Lin', 35), (3, 'Grace', 85)");

    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT name FROM people WHERE id = 2"),
              "name\n'Lin'");
    EXPECT_NE(fixture.executor->ExecuteQuery("EXPLAIN SELECT * FROM people WHERE id = 2")
                  .find("IndexPointLookup"),
              std::string::npos);
    const std::string analyzed =
        fixture.executor->ExecuteQuery("EXPLAIN ANALYZE SELECT * FROM people WHERE id = 2");
    EXPECT_NE(analyzed.find("Actual rows: 1"), std::string::npos);
    EXPECT_NE(analyzed.find("wall_ms="), std::string::npos);
    EXPECT_NE(analyzed.find("pages_fetched="), std::string::npos);
    EXPECT_NE(analyzed.find("IndexPointLookup"), std::string::npos);
    EXPECT_NE(fixture.executor->ExecuteQuery("EXPLAIN SELECT * FROM people WHERE age >= 36")
                  .find("SeqScan"),
              std::string::npos);
    fixture.bpm->AssertNoPinsHeld();
}

TEST(SqlExecutionTest, FailedMultiRowInsertRollsBackEveryRow) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery("CREATE TABLE items (id INT PRIMARY KEY, value VARCHAR(32))");

    EXPECT_THROW(fixture.executor->ExecuteQuery(
                     "INSERT INTO items VALUES (1, 'one'), (2, 'two'), (1, 'duplicate')"),
                 DbError);
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT COUNT(*) FROM items"), "COUNT(*)\n0");
    fixture.bpm->AssertNoPinsHeld();
}

TEST(SqlExecutionTest, ExecutesFiltersRangesProjectionSortAndLimits) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery(
        "CREATE TABLE scores (id INT PRIMARY KEY, category VARCHAR(20), score INT)");
    fixture.executor->ExecuteQuery(
        "INSERT INTO scores VALUES (1, 'blue', 7), (2, 'red', 4), (3, 'blue', 9), "
        "(4, 'green', NULL)");
    fixture.executor->ExecuteQuery("CREATE INDEX scores_score_idx ON scores (score)");

    EXPECT_NE(
        fixture.executor->ExecuteQuery("EXPLAIN SELECT id FROM scores WHERE id BETWEEN 2 AND 3")
            .find("IndexRangeScan"),
        std::string::npos);
    EXPECT_EQ(fixture.executor->ExecuteQuery(
                  "SELECT id, category FROM scores WHERE score IS NULL OR category LIKE 'bl%' "
                  "ORDER BY id DESC LIMIT 2 OFFSET 0"),
              "id\tcategory\n4\t'green'\n3\t'blue'");
    EXPECT_EQ(fixture.executor->ExecuteQuery(
                  "SELECT id FROM scores WHERE id IN (1, 3) AND NOT score < 8 ORDER BY id"),
              "id\n3");
    EXPECT_NE(fixture.executor->ExecuteQuery("EXPLAIN SELECT id FROM scores ORDER BY id DESC")
                  .find("ordered index"),
              std::string::npos);
    fixture.bpm->AssertNoPinsHeld();
}

TEST(SqlExecutionTest, AggregatesUpdateAndDeletePreserveIndexes) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery(
        "CREATE TABLE totals (id INT PRIMARY KEY, amount BIGINT, family VARCHAR(20) NOT NULL)");
    fixture.executor->ExecuteQuery(
        "INSERT INTO totals VALUES (1, 10, 'a'), (2, 20, 'a'), (3, 5, 'b')");
    fixture.executor->ExecuteQuery("CREATE INDEX totals_amount_idx ON totals (amount)");

    EXPECT_EQ(fixture.executor->ExecuteQuery(
                  "SELECT family, COUNT(*), SUM(amount), MIN(amount), MAX(amount), AVG(amount) "
                  "FROM totals GROUP BY family"),
              "family\tCOUNT(*)\tSUM(amount)\tMIN(amount)\tMAX(amount)\tAVG(amount)\n"
              "'a'\t2\t30\t10\t20\t15\n'b'\t1\t5\t5\t5\t5");
    EXPECT_EQ(fixture.executor->ExecuteQuery("UPDATE totals SET id = 4, amount = amount + 1 "
                                             "WHERE id = 1"),
              "1 row(s) affected");
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT id, amount FROM totals WHERE id = 4"),
              "id\tamount\n4\t11");
    EXPECT_EQ(fixture.executor->ExecuteQuery("DELETE FROM totals WHERE amount BETWEEN 5 AND 6"),
              "1 row(s) affected");
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT COUNT(*) FROM totals"), "COUNT(*)\n2");
    EXPECT_NE(fixture.executor
                  ->ExecuteQuery("EXPLAIN ANALYZE SELECT amount FROM totals WHERE amount >= 11")
                  .find("pages_fetched="),
              std::string::npos);
    EXPECT_EQ(fixture.executor->ExecuteQuery(
                  "SELECT family, SUM(amount) AS total FROM totals GROUP BY family "
                  "ORDER BY total DESC LIMIT 1"),
              "family\ttotal\n'a'\t31");
    fixture.bpm->AssertNoPinsHeld();
}

TEST(SqlExecutionTest, CatalogAndRowsSurviveRestart) {
    TemporaryDatabase database;
    {
        SqlFixture fixture(database.Path());
        fixture.executor->ExecuteQuery(
            "CREATE TABLE persisted (id BIGINT PRIMARY KEY, ok BOOLEAN)");
        fixture.executor->ExecuteQuery("INSERT INTO persisted VALUES (9223372036854775807, TRUE)");
    }
    {
        SqlFixture fixture(database.Path());
        EXPECT_EQ(fixture.executor->ExecuteQuery(
                      "SELECT id, ok FROM persisted WHERE id = 9223372036854775807"),
                  "id\tok\n9223372036854775807\tTRUE");
        fixture.bpm->AssertNoPinsHeld();
    }
}

TEST(SqlExecutionTest, MovingUpdateMaintainsEveryIndexAndDropRemovesTable) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery(
        "CREATE TABLE movable (id INT PRIMARY KEY, payload VARCHAR(255))");
    fixture.executor->ExecuteQuery("CREATE INDEX movable_payload_idx ON movable (payload)");
    for (int32_t id = 0; id < 20; ++id) {
        fixture.executor->ExecuteQuery("INSERT INTO movable VALUES (" + std::to_string(id) + ", '" +
                                       std::string(180, 'x') + "')");
    }

    const std::string replacement(250, 'y');
    EXPECT_EQ(fixture.executor->ExecuteQuery("UPDATE movable SET payload = '" + replacement +
                                             "' WHERE id = 0"),
              "1 row(s) affected");
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT id FROM movable WHERE payload = '" +
                                             replacement + "'"),
              "id\n0");
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT COUNT(*) FROM movable WHERE payload = '" +
                                             std::string(180, 'x') + "'"),
              "COUNT(*)\n19");
    EXPECT_EQ(fixture.executor->ExecuteQuery("DROP TABLE movable"), "Table dropped");
    EXPECT_THROW(fixture.executor->ExecuteQuery("SELECT * FROM movable"), DbError);
    fixture.bpm->AssertNoPinsHeld();
}

TEST(SqlExecutionTest, FailedUpdateRollsBackEarlierRowsAndIndexes) {
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery(
        "CREATE TABLE unique_values (id INT PRIMARY KEY, code INT NOT NULL)");
    fixture.executor->ExecuteQuery("CREATE UNIQUE INDEX unique_code ON unique_values (code)");
    fixture.executor->ExecuteQuery("INSERT INTO unique_values VALUES (1, 10), (2, 20)");

    EXPECT_THROW(fixture.executor->ExecuteQuery("UPDATE unique_values SET code = 40 WHERE id >= 1"),
                 DbError);
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT id, code FROM unique_values ORDER BY id"),
              "id\tcode\n1\t10\n2\t20");
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT id FROM unique_values WHERE code = 10"),
              "id\n1");
    fixture.bpm->AssertNoPinsHeld();
}

TEST(ConcurrencyTest, SqlExecutorStatementGateTsanStress) {
    constexpr uint64_t seed = 0x51A7EULL;
    std::cout << "SQL statement-gate stress seed=" << seed << '\n';
    TemporaryDatabase database;
    SqlFixture fixture(database.Path());
    fixture.executor->ExecuteQuery("CREATE TABLE concurrent_rows (id INT PRIMARY KEY)");

    std::atomic<bool> succeeded{true};
    std::vector<int32_t> insert_ids(80);
    for (size_t index = 0; index < insert_ids.size(); ++index) {
        insert_ids[index] = static_cast<int32_t>(index);
    }
    std::mt19937_64 random(seed);
    std::shuffle(insert_ids.begin(), insert_ids.end(), random);

    std::vector<std::thread> readers;
    for (size_t reader = 0; reader < 4U; ++reader) {
        readers.emplace_back([&fixture, &succeeded]() {
            try {
                for (size_t iteration = 0; iteration < 80U; ++iteration) {
                    fixture.executor->ExecuteQuery("SELECT COUNT(*) FROM concurrent_rows");
                }
            } catch (const std::exception&) {
                succeeded.store(false);
            }
        });
    }
    std::thread writer([&fixture, &succeeded, &insert_ids]() {
        try {
            for (int32_t id : insert_ids) {
                fixture.executor->ExecuteQuery("INSERT INTO concurrent_rows VALUES (" +
                                               std::to_string(id) + ")");
            }
        } catch (const std::exception&) {
            succeeded.store(false);
        }
    });

    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_TRUE(succeeded.load());
    EXPECT_EQ(fixture.executor->ExecuteQuery("SELECT COUNT(*) FROM concurrent_rows"),
              "COUNT(*)\n80");
    fixture.bpm->AssertNoPinsHeld();
}

}  // namespace
}  // namespace relational_db
