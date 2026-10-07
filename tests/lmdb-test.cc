#include "testing.h"

#include "valtz/db/lmdb.h"

#include <format>

using namespace valtz;

TEST(lmdb, put_get_and_dupsort)
{
  auto dir = test::temp_dir("lmdb");
  auto env = db::Env::open(dir / "t.lmdb");
  REQUIRE_OK(env);
  auto kv = (*env)->open_dbi("kv");
  auto dup = (*env)->open_dbi("dup", true);
  REQUIRE_OK(kv);
  REQUIRE_OK(dup);
  REQUIRE_OK((*env)->write([&](db::Txn& t) -> Status {
    VALTZ_TRY(t.put(*kv, db::as_bytes("a"), db::as_bytes("1")));
    VALTZ_TRY(t.put(*dup, db::as_bytes("k"), db::as_bytes("x")));
    VALTZ_TRY(t.put(*dup, db::as_bytes("k"), db::as_bytes("y")));
    return ok_status();
  }));
  std::string got;
  int dups = 0;
  REQUIRE_OK((*env)->read([&](db::Txn& t) -> Status {
    VALTZ_ASSIGN(auto v, t.get(*kv, db::as_bytes("a")));
    if (v) {
      got = std::string(db::as_view(*v));
    }
    VALTZ_ASSIGN(db::Cursor c, t.cursor(*dup));
    for (auto e = c.seek_exact(db::as_bytes("k")); e; e = c.next_dup()) {
      ++dups;
    }
    return ok_status();
  }));
  CHECK(got == "1");
  CHECK(dups == 2);
}

TEST(lmdb, map_grows_instead_of_failing)
{
  auto dir = test::temp_dir("lmdb-grow");
  db::Env::Options o;
  o.initial_map_bytes = 1 << 20;  // 1 MB, far too small
  auto env = db::Env::open(dir / "g.lmdb", o);
  REQUIRE_OK(env);
  auto kv = (*env)->open_dbi("kv");
  REQUIRE_OK(kv);
  std::string blob(64 * 1024, 'z');
  for (int i = 0; i < 64; ++i) {  // 4 MB of values
    REQUIRE_OK((*env)->write([&](db::Txn& t) -> Status {
      auto key = std::format("k{:04}", i);
      return t.put(*kv, db::as_bytes(key), db::as_bytes(blob));
    }));
  }
  CHECK((*env)->map_bytes() > (std::size_t{1} << 20));
}
