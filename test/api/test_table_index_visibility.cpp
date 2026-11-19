#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/index_entry.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

using namespace duckdb; // NOLINT

namespace {

DuckTableEntry &GetTable(Connection &con, const Identifier &table_name) {
	auto &table =
	    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, table_name));
	return table.Cast<DuckTableEntry>();
}

//! The indexes of the table as the transaction of the connection sees them
vector<shared_ptr<IndexEntry>> VisibleIndexes(Connection &con, const Identifier &table_name) {
	auto &table = GetTable(con, table_name);
	return table.GetIndexes(table.schema.GetCatalogTransaction(*con.context));
}

vector<string> SortedNames(const vector<shared_ptr<IndexEntry>> &indexes) {
	vector<string> names;
	for (auto &index : indexes) {
		if (index->GetBindState() == IndexBindState::RETIRED) {
			names.emplace_back("<retired>");
		} else {
			names.push_back(index->GetName().GetIdentifierName());
		}
	}
	std::sort(names.begin(), names.end());
	return names;
}

vector<string> VisibleIndexNames(Connection &con, const Identifier &table_name) {
	return SortedNames(VisibleIndexes(con, table_name));
}

//! The indexes that the table maintains, whoever sees them
vector<string> MaintainedIndexNames(Connection &con, const Identifier &table_name) {
	vector<shared_ptr<IndexEntry>> indexes;
	for (auto index : GetTable(con, table_name).GetStorage().GetDataTableInfo()->GetIndexes().IndexEntries()) {
		indexes.push_back(std::move(index));
	}
	return SortedNames(indexes);
}

//! Starts a transaction with a snapshot of the database as it is now
void BeginSnapshot(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM t"));
}

} // namespace

TEST_CASE("Test the indexes of a table as a transaction sees them", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection other(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER, j INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT range, range FROM range(1000)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE unrelated(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE INDEX unrelated_idx ON unrelated(i)"));

	SECTION("an index of an uncommitted transaction is maintained, but only its transaction sees it") {
		REQUIRE_NO_FAIL(other.Query("BEGIN"));
		REQUIRE_NO_FAIL(other.Query("CREATE INDEX idx ON t(i)"));

		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE(VisibleIndexNames(other, "t") == vector<string> {"idx"});
		REQUIRE(MaintainedIndexNames(con, "t") == vector<string> {"idx"});

		// committing the index does not show it to a transaction with an older snapshot
		REQUIRE_NO_FAIL(other.Query("COMMIT"));
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx"});
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
	}

	SECTION("a rolled back index is retired and no longer maintained") {
		REQUIRE_NO_FAIL(other.Query("BEGIN"));
		REQUIRE_NO_FAIL(other.Query("CREATE INDEX idx ON t(i)"));
		auto indexes = VisibleIndexes(other, "t");
		REQUIRE(indexes.size() == 1);
		REQUIRE(indexes[0]->GetBindState() == IndexBindState::BOUND);
		REQUIRE_NO_FAIL(other.Query("ROLLBACK"));

		REQUIRE(indexes[0]->GetBindState() == IndexBindState::RETIRED);
		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE(MaintainedIndexNames(con, "t").empty());
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
	}

	SECTION("a dropped index stays usable for a transaction with an older snapshot") {
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx ON t(i)"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx2 ON t(j)"));

		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx", "idx2"});

		// the drop is not visible while it is uncommitted
		REQUIRE_NO_FAIL(other.Query("BEGIN"));
		REQUIRE_NO_FAIL(other.Query("DROP INDEX idx"));
		REQUIRE(VisibleIndexNames(other, "t") == vector<string> {"idx2"});
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx", "idx2"});
		REQUIRE(MaintainedIndexNames(con, "t") == vector<string> {"idx", "idx2"});

		// the committed drop only takes the index out of the indexes that the table maintains
		REQUIRE_NO_FAIL(other.Query("COMMIT"));
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx", "idx2"});
		REQUIRE(MaintainedIndexNames(con, "t") == vector<string> {"idx2"});
		shared_ptr<IndexEntry> dropped;
		shared_ptr<IndexEntry> kept;
		for (auto &index : VisibleIndexes(con, "t")) {
			REQUIRE(index->GetBindState() == IndexBindState::BOUND);
			REQUIRE_NOTHROW(index->GetReadHandle<BoundIndex>());
			(index->GetName().GetIdentifierName() == "idx" ? dropped : kept) = index;
		}
		REQUIRE(dropped);
		REQUIRE(kept);

		// later commits no longer reach the dropped index, which the older snapshot does not see anyway
		auto dropped_size = dropped->GetInMemorySize();
		auto kept_size = kept->GetInMemorySize();
		REQUIRE_NO_FAIL(other.Query("INSERT INTO t SELECT range, range FROM range(1000, 200000)"));
		REQUIRE(dropped->GetInMemorySize() == dropped_size);
		REQUIRE(kept->GetInMemorySize() > kept_size);
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx", "idx2"});
		REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM t WHERE i = 42"));

		// the index is destroyed once no transaction sees its catalog entry
		REQUIRE(dropped->GetBindState() == IndexBindState::BOUND);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		REQUIRE(dropped->GetBindState() == IndexBindState::RETIRED);
		REQUIRE_THROWS(dropped->GetReadHandle<BoundIndex>());
		REQUIRE(kept->GetBindState() == IndexBindState::BOUND);

		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t") == vector<string> {"idx2"});
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
	}

	SECTION("an altered index entry keeps its index") {
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx ON t(i)"));
		BeginSnapshot(con);
		auto before = VisibleIndexes(con, "t");
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		REQUIRE_NO_FAIL(con.Query("COMMENT ON INDEX idx IS 'altered'"));
		BeginSnapshot(con);
		auto after = VisibleIndexes(con, "t");
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		REQUIRE(before.size() == 1);
		REQUIRE(after.size() == 1);
		REQUIRE(before[0].get() == after[0].get());
	}

	SECTION("the index of a constraint belongs to the table versions that have the constraint") {
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx ON t(j)"));
		REQUIRE_NO_FAIL(con.Query("DROP INDEX idx"));

		REQUIRE_NO_FAIL(other.Query("BEGIN"));
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t ADD PRIMARY KEY (i)"));

		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE(VisibleIndexNames(other, "t").size() == 1);
		REQUIRE(MaintainedIndexNames(con, "t").size() == 1);
		auto primary_key = VisibleIndexes(other, "t")[0];

		// later versions of the table take the index over
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t ADD COLUMN k INTEGER"));
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t RENAME TO t2"));
		REQUIRE(VisibleIndexes(other, "t2").size() == 1);
		REQUIRE(VisibleIndexes(other, "t2")[0].get() == primary_key.get());
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t2 RENAME TO t"));

		REQUIRE_NO_FAIL(other.Query("COMMIT"));
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		BeginSnapshot(con);
		REQUIRE(VisibleIndexes(con, "t").size() == 1);
		REQUIRE(VisibleIndexes(con, "t")[0].get() == primary_key.get());
		REQUIRE(primary_key->GetBindState() == IndexBindState::BOUND);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		REQUIRE_FAIL(con.Query("INSERT INTO t VALUES (1, 1, 1)"));
	}

	SECTION("rolling back an added constraint retires its index") {
		REQUIRE_NO_FAIL(other.Query("BEGIN"));
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t ADD PRIMARY KEY (i)"));
		REQUIRE_NO_FAIL(other.Query("ALTER TABLE t ADD COLUMN k INTEGER"));
		auto primary_key = VisibleIndexes(other, "t")[0];
		REQUIRE_NO_FAIL(other.Query("ROLLBACK"));

		REQUIRE(primary_key->GetBindState() == IndexBindState::RETIRED);
		BeginSnapshot(con);
		REQUIRE(VisibleIndexNames(con, "t").empty());
		REQUIRE(MaintainedIndexNames(con, "t").empty());
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 1)"));
	}
}

TEST_CASE("Test an index entry without physical index", "[api]") {
	// the entry of a catalog entry whose index is not built or loaded yet
	auto pending = make_shared_ptr<IndexEntry>(42, ConstraintCheckMode::DEFAULT);
	REQUIRE(pending->GetBindState() == IndexBindState::PENDING);
	REQUIRE(pending->GetIndexOid() == 42);
	REQUIRE_THROWS(pending->GetReadHandle<BoundIndex>());

	// rolling back or dropping its catalog entry retires it, and it never gets an index
	pending->Retire();
	REQUIRE(pending->GetBindState() == IndexBindState::RETIRED);
	REQUIRE_THROWS(pending->GetReadHandle<BoundIndex>());
	pending->Retire();
	REQUIRE(pending->GetBindState() == IndexBindState::RETIRED);
}

TEST_CASE("Test the indexes of a table after a checkpoint load and a WAL replay", "[api]") {
	auto path = TestCreatePath("table_index_visibility.db");
	DeleteDatabase(path);
	auto config = GetTestConfig();
	config->SetOptionByName("checkpoint_threshold", "1TB");

	{
		DuckDB db(path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER PRIMARY KEY, j INTEGER UNIQUE, k INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT range, range, range FROM range(1000)"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx ON t(k)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		// these are replayed from the WAL
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX replayed_idx ON t(j, k)"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE replayed(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO replayed SELECT range FROM range(1000)"));
		REQUIRE_NO_FAIL(con.Query("ALTER TABLE replayed ADD PRIMARY KEY (i)"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX dropped_idx ON replayed(i)"));
		REQUIRE_NO_FAIL(con.Query("DROP INDEX dropped_idx"));
	}
	{
		DuckDB db(path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		// the constraint indexes come first, in constraint order
		auto indexes = VisibleIndexes(con, "t");
		REQUIRE(indexes.size() == 4);
		REQUIRE(indexes[0]->GetName().GetIdentifierName() == "PRIMARY_t_0");
		REQUIRE(indexes[1]->GetName().GetIdentifierName() == "UNIQUE_t_1");
		REQUIRE(VisibleIndexNames(con, "t") == MaintainedIndexNames(con, "t"));
		REQUIRE(VisibleIndexes(con, "replayed").size() == 1);
		REQUIRE(VisibleIndexNames(con, "replayed") == MaintainedIndexNames(con, "replayed"));
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		REQUIRE_FAIL(con.Query("INSERT INTO t VALUES (1, 2000, 2000)"));
		REQUIRE_FAIL(con.Query("INSERT INTO replayed VALUES (1)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE(VisibleIndexes(con, "t").size() == 4);
		REQUIRE(VisibleIndexes(con, "replayed").size() == 1);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		REQUIRE_FAIL(con.Query("INSERT INTO replayed VALUES (1)"));
	}
	DeleteDatabase(path);
}

TEST_CASE("Test reading the indexes of a table while they are created, dropped and checkpointed", "[api]") {
	auto path = TestCreatePath("table_index_visibility_concurrent.db");
	DeleteDatabase(path);
	auto config = GetTestConfig();
	DuckDB db(path, config.get());
	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER, j INTEGER)"));
	REQUIRE_NO_FAIL(setup.Query("INSERT INTO t SELECT range, range FROM range(5000)"));
	REQUIRE_NO_FAIL(setup.Query("CREATE INDEX permanent_idx ON t(j)"));
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE u(i INTEGER, j INTEGER UNIQUE)"));
	REQUIRE_NO_FAIL(setup.Query("INSERT INTO u SELECT range, range FROM range(5000)"));

	static constexpr idx_t READER_COUNT = 4;
	static constexpr idx_t WRITER_ITERATIONS = 40;
	std::atomic<bool> finished {false};
	std::atomic<idx_t> indexes_created {0};
	std::atomic<idx_t> constraints_added {0};
	mutex failure_lock;
	string failure;
	auto fail = [&](const string &message) {
		lock_guard<mutex> guard(failure_lock);
		if (failure.empty()) {
			failure = message;
		}
	};
	// the writers conflict with each other: only an internal or fatal error is a failure
	auto run = [&](Connection &con, const string &query) {
		auto result = con.Query(query);
		if (!result->HasError()) {
			return true;
		}
		auto error_type = result->GetErrorObject().Type();
		if (error_type == ExceptionType::INTERNAL || error_type == ExceptionType::FATAL) {
			fail(query + ": " + result->GetError());
		}
		return false;
	};
	vector<std::thread> threads;

	// each transaction reads the indexes that it sees, and uses them
	for (idx_t reader = 0; reader < READER_COUNT; reader++) {
		threads.emplace_back([&]() {
			Connection con(db);
			while (!finished) {
				auto begin = con.Query("BEGIN");
				auto snapshot = con.Query("SELECT (SELECT count(*) FROM t) + (SELECT count(*) FROM u)");
				if (begin->HasError() || snapshot->HasError()) {
					fail("reader: " + begin->GetError() + snapshot->GetError());
					return;
				}
				try {
					for (auto table_name : {"t", "u"}) {
						for (idx_t repeat = 0; repeat < 20; repeat++) {
							auto indexes = VisibleIndexes(con, table_name);
							if (indexes.empty()) {
								fail("each table has an index that is never dropped");
							}
							for (auto &index : indexes) {
								// a later transaction can drop the index, but it stays usable for this one
								if (index->GetName().GetIdentifierName().empty()) {
									fail("a visible index without name");
								}
								auto handle = index->GetReadHandle<BoundIndex>();
								if (handle->GetIndexType().empty()) {
									fail("a visible index without type");
								}
							}
						}
					}
				} catch (std::exception &ex) {
					fail(string("reader: ") + ex.what());
					return;
				}
				auto commit = con.Query("COMMIT");
				if (commit->HasError()) {
					fail("reader: " + commit->GetError());
					return;
				}
			}
		});
	}
	// indexes are created, dropped and rolled back
	for (idx_t writer = 0; writer < 2; writer++) {
		threads.emplace_back([&, writer]() {
			Connection con(db);
			auto index_name = "idx_" + to_string(writer);
			for (idx_t i = 0; i < WRITER_ITERATIONS; i++) {
				if (run(con, "CREATE INDEX " + index_name + " ON t(i)")) {
					indexes_created++;
				}
				run(con, "COMMENT ON INDEX " + index_name + " IS 'altered'");
				run(con, "DROP INDEX IF EXISTS " + index_name);
				run(con, "BEGIN");
				run(con, "CREATE INDEX " + index_name + " ON t(i, j)");
				run(con, "ROLLBACK");
			}
		});
	}
	// a constraint is added and rolled back
	threads.emplace_back([&]() {
		Connection con(db);
		for (idx_t i = 0; i < WRITER_ITERATIONS; i++) {
			run(con, "BEGIN");
			if (run(con, "ALTER TABLE u ADD PRIMARY KEY (i)")) {
				constraints_added++;
			}
			run(con, "ALTER TABLE u ADD COLUMN k INTEGER");
			run(con, "ROLLBACK");
		}
	});
	// the indexes are maintained and checkpointed
	// FIXME: also insert into u - an insert that races with ALTER TABLE ADD PRIMARY KEY can invalidate the database
	threads.emplace_back([&]() {
		Connection con(db);
		for (idx_t i = 0; i < WRITER_ITERATIONS; i++) {
			auto base = to_string(100000 + i * 100);
			run(con, "INSERT INTO t SELECT " + base + " + range, " + base + " + range FROM range(100)");
			run(con, "CHECKPOINT");
		}
	});

	for (idx_t i = READER_COUNT; i < threads.size(); i++) {
		threads[i].join();
	}
	finished = true;
	for (idx_t i = 0; i < READER_COUNT; i++) {
		threads[i].join();
	}
	INFO(failure);
	REQUIRE(failure.empty());
	REQUIRE(indexes_created > 0);
	REQUIRE(constraints_added > 0);

	// the indexes are consistent with the tables
	REQUIRE_NO_FAIL(setup.Query("CHECKPOINT"));
	auto result = setup.Query("SELECT count(*) FROM t WHERE j = 100042");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE_FAIL(setup.Query("INSERT INTO u VALUES (-1, 42)"));
	DeleteDatabase(path);
}
