#include "testing.h"

#include "valtz/cache/cache-store.h"

#include <fstream>
#include <thread>

using namespace valtz;
using namespace valtz::cache;

namespace {

void
put(CacheStore& c, const std::string& key, std::size_t bytes)
{
  std::ofstream(c.staging_path(key), std::ios::binary)
      << std::string(bytes, 'x');
  auto r = c.commit(key);
  CHECK(r.ok());
}

}

TEST(cache, lru_eviction_under_budget_skips_pinned)
{
  auto root = test::temp_dir("cache");
  CacheStore::Options o;
  o.budget_bytes = 10'000;
  auto c = CacheStore::open(root, o);
  REQUIRE_OK(c);
  put(**c, "a.bin", 4000);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  put(**c, "b.bin", 4000);
  auto pin_a = (*c)->pin("a.bin");  // oldest, but in use
  REQUIRE(pin_a);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  put(**c, "c.bin", 4000);          // 12 KB > 10 KB: evict
  CHECK((*c)->find("a.bin").has_value());   // pinned survives
  CHECK(!(*c)->find("b.bin").has_value());  // LRU unpinned goes
  CHECK((*c)->find("c.bin").has_value());
  CHECK((*c)->size_bytes() <= o.budget_bytes);
}

TEST(cache, survives_reopen_and_wipes_scratch)
{
  auto root = test::temp_dir("cache");
  {
    auto c = CacheStore::open(root);
    REQUIRE_OK(c);
    put(**c, "k.bin", 100);
    auto s = (*c)->scratch_dir("vpipe");
    REQUIRE_OK(s);
    std::ofstream(*s / "junk") << "x";
    // An interrupted write leaves a staging file behind.
    std::ofstream((*c)->staging_path("half.bin")) << "x";
  }
  auto c = CacheStore::open(root);
  REQUIRE_OK(c);
  CHECK((*c)->find("k.bin").has_value());
  CHECK((*c)->size_bytes() == 100);
  CHECK(!std::filesystem::exists(root / "scratch/vpipe/junk"));
  CHECK(!std::filesystem::exists((*c)->staging_path("half.bin")));
}

TEST(cache, keys_are_content_derived_and_safe)
{
  auto h = ContentHash::of(std::string_view("abc"));
  auto k = CacheStore::key_for(h, "thumb-256.jpg");
  CHECK(CacheStore::valid_key(k));
  CHECK(k.starts_with(h.hex()));
  CHECK(!CacheStore::valid_key("../escape"));
  CHECK(!CacheStore::valid_key("a/b"));
  CHECK(!CacheStore::valid_key(""));
}
