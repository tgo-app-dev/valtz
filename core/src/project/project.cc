#include "valtz/project/project.h"

#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/media/probe.h"
#include "project/bookmark.h"
#include "project/keys.h"

#include <sys/mount.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

namespace valtz::project {

namespace fs = std::filesystem;

const char*
to_str(StaleReason r)
{
  switch (r) {
  case StaleReason::Fresh:         return "fresh";
  case StaleReason::NeverBuilt:    return "never built";
  case StaleReason::RecipeChanged: return "recipe changed";
  case StaleReason::InputChanged:  return "input changed";
  case StaleReason::InputStale:    return "input is stale";
  case StaleReason::InputMissing:  return "input missing";
  }
  return "?";
}

const char*
to_str(LinkState s)
{
  switch (s) {
  case LinkState::Ok:        return "ok";
  case LinkState::Relocated: return "relocated";
  case LinkState::Modified:  return "modified";
  case LinkState::Missing:   return "missing";
  }
  return "?";
}

bool
links_in_place(Placement p, media::MediaType t)
{
  switch (p) {
  case Placement::Copy: return false;
  case Placement::Link: return true;
  case Placement::Auto: return t == media::MediaType::Video;
  }
  return false;
}

Status
check_local_volume(const fs::path& path)
{
  std::error_code ec;
  fs::path probe = path;
  while (!probe.empty() && !fs::exists(probe, ec)) {
    probe = probe.parent_path();
  }
  // Judge the full requested path (weakly_canonical resolves symlinks in
  // the existing prefix and keeps the rest), not just the part that
  // exists yet.
  fs::path real = fs::weakly_canonical(fs::absolute(path, ec), ec);
  const std::string s = real.string();
  if (s.find("/Library/Mobile Documents/") != std::string::npos ||
      s.find("/Library/CloudStorage/") != std::string::npos) {
    return make_error(Code::Unsupported, std::format(
        "{} is in a cloud-synced folder. A project's database is memory-"
        "mapped and a sync client can corrupt it; keep projects on a local "
        "folder and share them with Export.", path.string()));
  }
  struct statfs st;
  if (statfs(probe.c_str(), &st) == 0 && !(st.f_flags & MNT_LOCAL)) {
    return make_error(Code::Unsupported, std::format(
        "{} is on a network volume ({}). Projects must be on a local "
        "disk.", path.string(), st.f_fstypename));
  }
  return ok_status();
}

// ---- open / create ----------------------------------------------------

Project::~Project() = default;

Status
Project::open_dbs_()
{
  VALTZ_ASSIGN(_db.meta, _env->open_dbi("meta"));
  VALTZ_ASSIGN(_db.assets, _env->open_dbi("assets"));
  VALTZ_ASSIGN(_db.versions, _env->open_dbi("versions"));
  VALTZ_ASSIGN(_db.recipes, _env->open_dbi("recipes"));
  VALTZ_ASSIGN(_db.dependents, _env->open_dbi("dependents", true));
  VALTZ_ASSIGN(_db.subjects, _env->open_dbi("subjects"));
  VALTZ_ASSIGN(_db.subject_refs, _env->open_dbi("subject_refs", true));
  VALTZ_ASSIGN(_db.history, _env->open_dbi("history"));
  VALTZ_ASSIGN(_db.x_tables, _env->open_dbi("x_tables"));
  VALTZ_ASSIGN(_db.undo, _env->open_dbi("undo"));
  return ok_status();
}

namespace {

constexpr std::array<const char*, 9> kRecordTables = {
    "meta", "assets", "versions", "recipes", "dependents", "subjects",
    "subject_refs", "history", "x_tables"};

}

std::string
Project::table_name(unsigned handle) const
{
  const auto ts = record_tables();
  for (std::size_t i = 0; i < ts.size(); ++i) {
    if (ts[i].handle == handle) {
      return kRecordTables[i];
    }
  }
  return "";
}

std::optional<db::Dbi>
Project::table(std::string_view name) const
{
  const auto ts = record_tables();
  for (std::size_t i = 0; i < ts.size(); ++i) {
    if (name == kRecordTables[i]) {
      return ts[i];
    }
  }
  return std::nullopt;
}

std::vector<db::Dbi>
Project::record_tables() const
{
  return {_db.meta,     _db.assets,   _db.versions,     _db.recipes,
          _db.dependents, _db.subjects, _db.subject_refs, _db.history,
          _db.x_tables};
}

Result<std::unique_ptr<Project>>
Project::create(const fs::path& package, std::string name)
{
  VALTZ_TRY(check_local_volume(package));
  std::error_code ec;
  if (fs::exists(package / "project.lmdb", ec)) {
    return make_error(Code::AlreadyExists,
                      std::format("{} already exists", package.string()));
  }
  for (const char* sub : {"blobs", "tmp"}) {
    fs::create_directories(package / sub, ec);
    if (ec) {
      return make_error(Code::Io, std::format("create {}: {}",
                                              package.string(),
                                              ec.message()));
    }
  }

  std::unique_ptr<Project> p(new Project());
  p->_pkg = package;
  p->_blobs = BlobStore(package / "blobs");
  VALTZ_ASSIGN(p->_env, db::Env::open(package / "project.lmdb"));
  VALTZ_TRY(p->open_dbs_());
  p->_id = ProjectId::make();
  p->_name = std::move(name);

  Json meta = {
    {"schema", kSchemaVersion},
    {"id", p->_id},
    {"name", p->_name},
    {"created", now_ms()},
    {"app_version", VALTZ_VERSION},
  };
  VALTZ_TRY(p->_env->write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, p->_db.meta, db::as_bytes("project"), meta);
  }));
  VALTZ_LOG_INFO("project", "created {} ({})", p->_name, p->_id.str());
  return p;
}

Result<std::unique_ptr<Project>>
Project::open(const fs::path& package)
{
  VALTZ_TRY(check_local_volume(package));
  std::error_code ec;
  if (!fs::is_regular_file(package / "project.lmdb", ec)) {
    return make_error(Code::NotFound,
                      std::format("{} is not a Valtz project",
                                  package.string()));
  }
  std::unique_ptr<Project> p(new Project());
  p->_pkg = package;
  p->_blobs = BlobStore(package / "blobs");
  VALTZ_ASSIGN(p->_env, db::Env::open(package / "project.lmdb"));
  VALTZ_TRY(p->open_dbs_());

  Json meta;
  VALTZ_TRY(p->_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto m, get_cbor(txn, p->_db.meta,
                                  db::as_bytes("project")));
    if (!m) {
      return make_error(Code::Corrupt, "project metadata missing");
    }
    meta = std::move(*m);
    return ok_status();
  }));
  auto schema = jget<std::uint32_t>(meta, "schema", 0);
  if (schema > kSchemaVersion) {
    return make_error(Code::Unsupported, std::format(
        "project was written by a newer Valtz (schema {}, this build "
        "reads {})", schema, kSchemaVersion));
  }
  p->_id = jget(meta, "id", ProjectId{});
  p->_name = jget<std::string>(meta, "name", package.stem().string());
  return p;
}

Status
Project::set_name(std::string name)
{
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto m, get_cbor(txn, _db.meta, db::as_bytes("project")));
    Json meta = m ? std::move(*m) : Json::object();
    meta["name"] = name;
    return put_cbor(txn, _db.meta, db::as_bytes("project"), meta);
  }));
  _name = std::move(name);
  return ok_status();
}

// ---- record helpers ---------------------------------------------------

Result<std::optional<Asset>>
Project::get_asset_(const db::Txn& txn, AssetId id) const
{
  VALTZ_ASSIGN(auto j, get_cbor(txn, _db.assets, id_key(id)));
  if (!j) {
    return std::optional<Asset>{};
  }
  return std::optional<Asset>{j->get<Asset>()};
}

Result<std::optional<AssetVersion>>
Project::get_version_(const db::Txn& txn, AssetId id,
                      std::uint32_t n) const
{
  auto key = version_key(id, n);
  VALTZ_ASSIGN(auto j, get_cbor(txn, _db.versions, key));
  if (!j) {
    return std::optional<AssetVersion>{};
  }
  return std::optional<AssetVersion>{j->get<AssetVersion>()};
}

Result<std::optional<Recipe>>
Project::get_recipe_(const db::Txn& txn, RecipeId id) const
{
  VALTZ_ASSIGN(auto j, get_cbor(txn, _db.recipes, id_key(id)));
  if (!j) {
    return std::optional<Recipe>{};
  }
  return std::optional<Recipe>{j->get<Recipe>()};
}

Result<std::uint32_t>
Project::append_version_(db::Txn& txn, Asset& a, AssetVersion& v)
{
  v.asset = a.id;
  v.number = a.head + 1;
  v.created_ms = now_ms();
  VALTZ_TRY(put_cbor(txn, _db.versions, version_key(a.id, v.number),
                     Json(v)));
  a.head = v.number;
  a.modified_ms = v.created_ms;
  VALTZ_TRY(put_cbor(txn, _db.assets, id_key(a.id), Json(a)));
  return v.number;
}

// ---- assets -----------------------------------------------------------

Result<Asset>
Project::import_file(const fs::path& src, const ImportOptions& opts)
{
  VALTZ_ASSIGN(media::MediaInfo info, media::probe_file(src));
  const bool link = links_in_place(opts.placement, info.type);

  Asset a;
  a.id = AssetId::make();
  a.name = opts.name.empty() ? src.filename().string() : opts.name;
  a.kind = asset_kind_for(info.type);
  a.origin = Origin::Source;
  a.created_ms = now_ms();
  a.source_path = fs::absolute(src).string();
  a.linked = link;

  AssetVersion v;
  v.info = info;
  if (link) {
    // Stamp before hashing: if the file changes while we read it, the
    // next check sees a newer mtime and re-hashes.
    auto stamp = stamp_of(src);
    if (!stamp) {
      return make_error(Code::NotFound,
                        std::format("cannot stat {}", src.string()));
    }
    VALTZ_ASSIGN(v.content, hash_file(src));
    a.link = {make_bookmark(src), stamp->size, stamp->mtime_ns};
  } else {
    VALTZ_ASSIGN(v.blob, _blobs.ingest_file(src));
    v.content = v.blob.hash;
  }

  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    Asset rec = a;  // write() may re-run fn; mutate a copy
    AssetVersion ver = v;
    VALTZ_TRY(append_version_(txn, rec, ver));
    return ok_status();
  }));
  a.head = 1;
  a.modified_ms = a.created_ms;
  VALTZ_LOG_INFO("project", "imported {} as {} {} ({}, {}x{}, {})", a.name,
                 link ? "linked" : "copied", to_str(a.kind),
                 info.codec_name, info.frame.width, info.frame.height,
                 info.frame.color.describe());
  return a;
}

Result<LinkCheck>
Project::check_link(AssetId id) const
{
  VALTZ_ASSIGN(Asset a, asset(id));
  if (!a.linked) {
    return make_error(Code::InvalidArgument, "asset is not linked");
  }
  auto same = [&](const fs::path& p) {
    auto st = stamp_of(p);
    return st && st->size == a.link.size && st->mtime_ns == a.link.mtime_ns;
  };
  std::error_code ec;
  if (fs::is_regular_file(a.source_path, ec)) {
    return LinkCheck{same(a.source_path) ? LinkState::Ok
                                         : LinkState::Modified,
                     a.source_path};
  }
  if (auto moved = resolve_bookmark(a.link.bookmark)) {
    return LinkCheck{same(*moved) ? LinkState::Relocated
                                  : LinkState::Modified,
                     *moved};
  }
  return LinkCheck{LinkState::Missing, {}};
}

Result<LinkCheck>
Project::refresh_link(AssetId id)
{
  VALTZ_ASSIGN(LinkCheck chk, check_link(id));
  if (chk.state == LinkState::Ok || chk.state == LinkState::Missing) {
    return chk;
  }
  auto stamp = stamp_of(chk.path);
  if (!stamp) {
    return LinkCheck{LinkState::Missing, {}};
  }
  // Relocated: only the path and bookmark change. Modified: re-hash; an
  // unchanged hash (touched, copied back) is still not a new version.
  std::optional<AssetVersion> fresh;
  if (chk.state == LinkState::Modified) {
    VALTZ_ASSIGN(ContentHash h, hash_file(chk.path));
    VALTZ_ASSIGN(AssetVersion head, version(id));
    if (h != head.content) {
      VALTZ_ASSIGN(media::MediaInfo info, media::probe_file(chk.path));
      AssetVersion v;
      v.info = info;
      v.content = h;
      fresh = v;
    }
  }
  std::string bookmark = make_bookmark(chk.path);
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->source_path = chk.path.string();
    a->link = {bookmark.empty() ? a->link.bookmark : bookmark, stamp->size,
               stamp->mtime_ns};
    if (fresh) {
      AssetVersion v = *fresh;
      VALTZ_TRY(append_version_(txn, *a, v));  // also writes the asset
      return ok_status();
    }
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  }));
  VALTZ_LOG_INFO("project", "linked source {} {}{}", chk.path.string(),
                 to_str(chk.state), fresh ? " -> new version" : "");
  return chk;
}

Result<Asset>
Project::add_text(std::string name, std::string_view text,
                  std::vector<std::string> tags)
{
  VALTZ_ASSIGN(BlobRef blob, _blobs.put_bytes(text, "txt"));
  Asset a;
  a.id = AssetId::make();
  a.name = std::move(name);
  a.tags = std::move(tags);
  a.kind = AssetKind::Text;
  a.origin = Origin::Source;
  a.created_ms = now_ms();

  AssetVersion v;
  v.blob = blob;
  v.content = blob.hash;
  v.info.type = media::MediaType::Text;
  v.info.uti = "public.utf8-plain-text";

  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    Asset rec = a;
    AssetVersion ver = v;
    VALTZ_TRY(append_version_(txn, rec, ver));
    return ok_status();
  }));
  a.head = 1;
  a.modified_ms = a.created_ms;
  return a;
}

Result<AssetVersion>
Project::update_text(AssetId id, std::string_view text)
{
  VALTZ_ASSIGN(BlobRef blob, _blobs.put_bytes(text, "txt"));
  AssetVersion out;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    if (a->origin != Origin::Source || a->kind != AssetKind::Text) {
      return make_error(Code::InvalidArgument,
                        "only source text assets are edited in place");
    }
    AssetVersion v;
    v.blob = blob;
    v.content = blob.hash;
    v.info.type = media::MediaType::Text;
    v.info.uti = "public.utf8-plain-text";
    VALTZ_TRY(append_version_(txn, *a, v));
    out = v;
    return ok_status();
  }));
  return out;
}

Result<Asset>
Project::asset(AssetId id) const
{
  Asset out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound,
                        std::format("no asset {}", id.str()));
    }
    out = std::move(*a);
    return ok_status();
  }));
  return out;
}

Result<std::vector<Asset>>
Project::assets() const
{
  std::vector<Asset> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    Status st;
    VALTZ_TRY(db::for_each_prefix(txn, _db.assets, {},
        [&](const db::Cursor::Entry& e) {
          auto j = from_cbor(e.value);
          if (!j.ok()) {
            st = j.error();
            return false;
          }
          out.push_back(j->get<Asset>());
          return true;
        }));
    return st;
  }));
  return out;
}

Result<AssetVersion>
Project::version(AssetId id, std::uint32_t number) const
{
  AssetVersion out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    std::uint32_t n = number;
    if (n == 0) {
      VALTZ_ASSIGN(auto a, get_asset_(txn, id));
      if (!a || a->head == 0) {
        return make_error(Code::NotFound, "asset has no version");
      }
      n = a->head;
    }
    VALTZ_ASSIGN(auto v, get_version_(txn, id, n));
    if (!v) {
      return make_error(Code::NotFound,
                        std::format("no version {} of {}", n, id.str()));
    }
    out = std::move(*v);
    return ok_status();
  }));
  return out;
}

Result<std::vector<AssetVersion>>
Project::versions(AssetId id) const
{
  std::vector<AssetVersion> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    auto prefix = id_key(id);
    return db::for_each_prefix(txn, _db.versions, prefix,
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            out.push_back(j->get<AssetVersion>());
          }
          return true;
        });
  }));
  return out;
}

Status
Project::rename_asset(AssetId id, std::string name)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->name = name;
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Status
Project::set_modifiers(AssetId id, std::vector<Modifier> mods)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->modifiers = std::move(mods);
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Status
Project::set_layers(AssetId id, std::vector<Layer> layers)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->layers = std::move(layers);
    tidy_layer_folders(*a);
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Status
Project::update_asset(AssetId id,
                      const std::function<Status(Asset&)>& edit)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    Asset b = *a;
    VALTZ_TRY(edit(b));
    tidy_layer_folders(b);
    b.id = a->id;
    b.kind = a->kind;
    b.head = a->head;
    b.recipe = a->recipe;
    b.origin = a->origin;
    b.modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(b));
  });
}

Result<Asset>
Project::add_asset(Asset a, std::size_t max_name_bytes)
{
  a.id = AssetId::make();
  a.origin = Origin::Source;
  a.head = 0;
  a.recipe = RecipeId{};
  a.created_ms = now_ms();
  a.modified_ms = a.created_ms;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(a.name, unique_name_(txn, a.name, max_name_bytes));
    return put_cbor(txn, _db.assets, id_key(a.id), Json(a));
  }));
  return a;
}

Result<std::optional<AssetId>>
Project::composition() const
{
  std::optional<AssetId> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto m, get_cbor(txn, _db.meta,
                                  db::as_bytes("composition")));
    if (m && m->is_string()) {
      if (auto id = AssetId::parse(m->get<std::string>()); id &&
          !id->is_nil()) {
        out = *id;
      }
    }
    return ok_status();
  }));
  return out;
}

Status
Project::set_composition(std::optional<AssetId> id)
{
  return _env->write([&](db::Txn& txn) -> Status {
    if (!id) {
      return txn.del(_db.meta, db::as_bytes("composition"));
    }
    return put_cbor(txn, _db.meta, db::as_bytes("composition"),
                    Json(id->str()));
  });
}

Result<std::uint32_t>
Project::schema() const
{
  std::uint32_t schema = 0;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto m, get_cbor(txn, _db.meta, db::as_bytes("project")));
    schema = m ? jget<std::uint32_t>(*m, "schema", 0) : 0;
    return ok_status();
  }));
  return schema;
}

Status
Project::set_canvas(AssetId id, const media::StackCanvas& canvas)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->canvas = canvas;
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Status
Project::set_timeline(AssetId id, std::int64_t frames)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->timeline_frames = std::max<std::int64_t>(0, frames);
    a->modified_ms = now_ms();
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Result<fs::path>
Project::media_path(AssetId id, std::uint32_t number) const
{
  VALTZ_ASSIGN(Asset a, asset(id));
  VALTZ_ASSIGN(AssetVersion v, version(id, number));
  if (!v.blob.empty()) {
    return _blobs.path_of(v.blob);
  }
  if (!a.linked) {
    return make_error(Code::NotFound, "asset version has no media");
  }
  if (number != 0 && number != a.head) {
    // Only the current bytes of a linked original exist anywhere.
    return make_error(Code::NotFound, std::format(
        "version {} of linked {} was replaced when the original changed",
        number, a.name));
  }
  VALTZ_ASSIGN(LinkCheck chk, check_link(id));
  switch (chk.state) {
  case LinkState::Ok:
  case LinkState::Relocated:
    return chk.path;
  case LinkState::Modified:
    return make_error(Code::Corrupt, std::format(
        "the original of {} changed since it was imported ({}); refresh "
        "it to take the new content", a.name, chk.path.string()));
  case LinkState::Missing:
    break;
  }
  return make_error(Code::NotFound, std::format(
      "the original of {} is missing ({}) -- is its drive connected?",
      a.name, a.source_path));
}

Result<std::string>
Project::read_text(AssetId id, std::uint32_t number) const
{
  VALTZ_ASSIGN(fs::path p, media_path(id, number));
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return make_error(Code::Io, std::format("cannot read {}", p.string()));
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---- derived assets ---------------------------------------------------

Result<std::string>
Project::unique_name_(const db::Txn& txn, std::string_view name,
                      std::size_t max_bytes) const
{
  std::unordered_set<std::string> taken;
  Status st;
  VALTZ_TRY(db::for_each_prefix(txn, _db.assets, {},
      [&](const db::Cursor::Entry& e) {
        auto j = from_cbor(e.value);
        if (!j.ok()) {
          st = j.error();
          return false;
        }
        taken.insert(jget<std::string>(*j, "name", ""));
        return true;
      }));
  VALTZ_TRY(st);
  if (std::string whole = utf8_truncate(name, max_bytes);
      !taken.contains(whole)) {
    return whole;
  }
  // Taken: numbered -- on from its own number when it has one ("Take
  // (2)" -> "Take (3)", never "Take (2) (2)").
  std::string_view stem = name;
  if (const auto open = name.rfind(" (");
      open != std::string_view::npos && name.size() > open + 3 &&
      name.back() == ')' &&
      std::ranges::all_of(name.substr(open + 2, name.size() - open - 3),
                          [](char c) { return c >= '0' && c <= '9'; })) {
    stem = name.substr(0, open);
  }
  for (std::size_t k = 2;; ++k) {
    const std::string suffix = std::format(" ({})", k);
    std::string base = max_bytes > suffix.size()
                           ? utf8_truncate(stem, max_bytes - suffix.size())
                           : std::string(stem);
    std::string candidate = std::move(base) + suffix;
    if (!taken.contains(candidate)) {
      return candidate;
    }
  }
}

Result<Asset>
Project::define_derived(std::string name, AssetKind kind, Recipe r,
                        std::size_t max_name_bytes)
{
  r.id = RecipeId::make();
  r.created_ms = now_ms();

  Asset a;
  a.id = AssetId::make();
  a.kind = kind;
  a.origin = Origin::Derived;
  a.recipe = r.id;
  a.created_ms = r.created_ms;
  a.modified_ms = r.created_ms;

  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(a.name, unique_name_(txn, name, max_name_bytes));
    for (const auto& in : r.inputs) {
      VALTZ_ASSIGN(auto ia, get_asset_(txn, in.asset));
      if (!ia) {
        return make_error(Code::NotFound, std::format(
            "recipe input '{}' refers to a missing asset", in.role));
      }
    }
    VALTZ_TRY(put_cbor(txn, _db.recipes, id_key(r.id), Json(r)));
    VALTZ_TRY(put_cbor(txn, _db.assets, id_key(a.id), Json(a)));
    for (const auto& in : r.inputs) {
      VALTZ_TRY(txn.put(_db.dependents, id_key(in.asset), id_key(a.id),
                        /*unique=*/false));
    }
    return ok_status();
  }));
  return a;
}

Status
Project::set_recipe(AssetId id, Recipe r)
{
  r.id = RecipeId::make();
  r.created_ms = now_ms();

  // A cycle would make "up to date" undefined. The new recipe's inputs
  // must not already depend on this asset.
  for (const auto& in : r.inputs) {
    if (in.asset == id) {
      return make_error(Code::InvalidArgument,
                        "an asset cannot be an input to itself");
    }
  }
  VALTZ_ASSIGN(auto downstream, dependents(id, /*transitive=*/true));
  for (const auto& in : r.inputs) {
    for (const auto& d : downstream) {
      if (d == in.asset) {
        return make_error(Code::InvalidArgument, std::format(
            "input '{}' already depends on this asset (cycle)", in.role));
      }
    }
  }

  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    if (a->origin != Origin::Derived) {
      return make_error(Code::InvalidArgument,
                        "only derived assets have recipes");
    }
    // Drop the old recipe's edges, add the new ones.
    if (!a->recipe.is_nil()) {
      VALTZ_ASSIGN(auto old, get_recipe_(txn, a->recipe));
      if (old) {
        for (const auto& in : old->inputs) {
          VALTZ_TRY(txn.del(_db.dependents, id_key(in.asset), id_key(id)));
        }
      }
    }
    VALTZ_TRY(put_cbor(txn, _db.recipes, id_key(r.id), Json(r)));
    for (const auto& in : r.inputs) {
      VALTZ_TRY(txn.put(_db.dependents, id_key(in.asset), id_key(id)));
    }
    a->recipe = r.id;
    a->modified_ms = r.created_ms;
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Result<Recipe>
Project::recipe(RecipeId id) const
{
  Recipe out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto r, get_recipe_(txn, id));
    if (!r) {
      return make_error(Code::NotFound, "no such recipe");
    }
    out = std::move(*r);
    return ok_status();
  }));
  return out;
}

Result<std::vector<ResolvedInput>>
Project::resolve_inputs_(const db::Txn& txn, const Recipe& r) const
{
  std::vector<ResolvedInput> out;
  for (const auto& in : r.inputs) {
    VALTZ_ASSIGN(auto a, get_asset_(txn, in.asset));
    if (!a) {
      return make_error(Code::NotFound, std::format(
          "input '{}' ({}) no longer exists", in.role, in.asset.str()));
    }
    // A composition or a markup has no version: what it draws, keyed.
    if (a->cls == AssetClass::Still || a->cls == AssetClass::Composition ||
        a->cls == AssetClass::Markup) {
      VALTZ_ASSIGN(ContentHash k, content_key_(txn, in.asset, 0, 0));
      out.push_back({in.role, in.asset, 0, k});
      continue;
    }
    std::uint32_t n = in.version ? in.version : a->head;
    if (n == 0) {
      return make_error(Code::NotFound, std::format(
          "input '{}' ({}) has not been built", in.role, a->name));
    }
    VALTZ_ASSIGN(auto v, get_version_(txn, in.asset, n));
    if (!v) {
      return make_error(Code::NotFound, std::format(
          "input '{}' version {} is missing", in.role, n));
    }
    out.push_back({in.role, in.asset, n, v->content});
  }
  return out;
}

Result<std::vector<ResolvedInput>>
Project::resolve_inputs(const Recipe& r) const
{
  std::vector<ResolvedInput> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(out, resolve_inputs_(txn, r));
    return ok_status();
  }));
  return out;
}

Result<AssetVersion>
Project::commit_build(AssetId id, BuildRecord b)
{
  std::string ext = b.output.extension().string();
  if (!ext.empty()) {
    ext.erase(0, 1);
  }
  VALTZ_ASSIGN(BlobRef blob, _blobs.adopt_file(b.output, ext));

  AssetVersion out;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    AssetVersion v;
    v.blob = blob;
    v.content = blob.hash;
    v.info = b.info;
    v.recipe = b.recipe.id;
    v.executed = b.recipe.params;
    v.inputs = b.inputs;
    v.fingerprint = fingerprint(b.recipe, b.inputs, b.model_digest);
    v.engine = b.engine;
    v.host = b.host;
    v.timing = b.timing;
    v.outputs = b.outputs;
    VALTZ_TRY(append_version_(txn, *a, v));
    out = v;
    return ok_status();
  }));
  return out;
}

// ---- the asset list: folders, shared versions, removal ----------------

Result<Json>
Project::folders() const
{
  Json out = Json::array();
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto j, get_cbor(txn, _db.meta, db::as_bytes("folders")));
    if (j && j->is_array()) {
      out = std::move(*j);
    }
    return ok_status();
  }));
  return out;
}

Status
Project::set_folders(const Json& folders)
{
  return _env->write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _db.meta, db::as_bytes("folders"), folders);
  });
}

Result<Json>
Project::view_state() const
{
  Json out = Json::object();
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto j, get_cbor(txn, _db.meta, db::as_bytes("view")));
    if (j && j->is_object()) {
      out = std::move(*j);
    }
    return ok_status();
  }));
  return out;
}

Result<OutputSettings>
Project::output() const
{
  OutputSettings out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto j, get_cbor(txn, _db.meta, db::as_bytes("output")));
    if (j && j->is_object()) {
      out = j->get<OutputSettings>();
    }
    return ok_status();
  }));
  return out;
}

Status
Project::set_output(const OutputSettings& o)
{
  return _env->write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _db.meta, db::as_bytes("output"), Json(o));
  });
}

Status
Project::set_view_state(const Json& view)
{
  return _env->write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _db.meta, db::as_bytes("view"), view);
  });
}

Status
Project::set_asset_folder(AssetId id, std::string folder)
{
  return _env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    a->folder = std::move(folder);
    return put_cbor(txn, _db.assets, id_key(id), Json(*a));
  });
}

Result<AssetVersion>
Project::share_version(AssetId dst, AssetId src, std::uint32_t number,
                       const Recipe& recipe,
                       const std::vector<ResolvedInput>& in)
{
  AssetVersion out;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, dst));
    VALTZ_ASSIGN(auto s, get_asset_(txn, src));
    if (!a || !s) {
      return make_error(Code::NotFound, "no such asset");
    }
    VALTZ_ASSIGN(auto sv, get_version_(txn, src, number ? number : s->head));
    if (!sv) {
      return make_error(Code::NotFound, "no such version");
    }
    AssetVersion v;
    v.blob = sv->blob;
    v.content = sv->content;
    v.info = sv->info;
    v.recipe = recipe.id;
    v.executed = recipe.params;
    v.inputs = in;
    v.fingerprint = fingerprint(recipe, in);
    // A linked original is no blob: the asset links the same file.
    if (sv->blob.empty()) {
      a->linked = s->linked;
      a->source_path = s->source_path;
      a->link = s->link;
    }
    VALTZ_TRY(append_version_(txn, *a, v));
    out = v;
    return ok_status();
  }));
  return out;
}

Status
Project::remove_asset(AssetId id)
{
  std::vector<BlobRef> blobs;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto a, get_asset_(txn, id));
    if (!a) {
      return make_error(Code::NotFound, "no such asset");
    }
    {
      VALTZ_ASSIGN(db::Cursor c, txn.cursor(_db.dependents));
      if (c.seek_exact(id_key(id))) {
        return make_error(Code::Busy, "other assets are built from it");
      }
    }
    // Its versions, and the recipes they were built by.
    std::vector<std::vector<std::uint8_t>> keys;
    std::set<RecipeId> recipes;
    if (!a->recipe.is_nil()) {
      recipes.insert(a->recipe);
    }
    VALTZ_TRY(db::for_each_prefix(txn, _db.versions, id_key(id),
        [&](const db::Cursor::Entry& e) {
          keys.emplace_back(e.key.begin(), e.key.end());
          if (auto j = from_cbor(e.value); j.ok()) {
            const auto v = j->get<AssetVersion>();
            if (!v.blob.empty()) {
              blobs.push_back(v.blob);
            }
            if (!v.recipe.is_nil()) {
              recipes.insert(v.recipe);
            }
          }
          return true;
        }));
    for (const auto& k : keys) {
      VALTZ_TRY(txn.del(_db.versions, db::Bytes(k.data(), k.size())));
    }
    for (const auto& rid : recipes) {
      VALTZ_ASSIGN(auto r, get_recipe_(txn, rid));
      if (r) {
        for (const auto& in : r->inputs) {
          (void)txn.del(_db.dependents, id_key(in.asset), id_key(id));
        }
      }
      (void)txn.del(_db.recipes, id_key(rid));
    }
    return txn.del(_db.assets, id_key(id));
  }));
  // In a working copy the blobs stay: the undo history may bring the
  // asset back, and a save collects what nothing holds.
  if (blobs.empty() || _keep_blobs) {
    return ok_status();
  }
  // The blobs nothing left holds: no version of another asset (one that
  // shares it), no history state, no markup.
  VALTZ_ASSIGN(const std::set<std::string> held, held_blobs());
  for (const auto& b : blobs) {
    if (!held.contains(b.hash.hex())) {
      (void)_blobs.remove(b);
    }
  }
  return ok_status();
}

Result<std::set<std::string>>
Project::held_blobs() const
{
  std::set<std::string> held;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_TRY(db::for_each_prefix(txn, _db.versions, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            if (const auto b = j->get<AssetVersion>().blob; !b.empty()) {
              held.insert(b.hash.hex());
            }
          }
          return true;
        }));
    VALTZ_TRY(db::for_each_prefix(txn, _db.history, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            if (const auto b = j->get<HistoryEntry>().picture; !b.empty()) {
              held.insert(b.hash.hex());
            }
          }
          return true;
        }));
    return db::for_each_prefix(txn, _db.assets, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            const Asset a = j->get<Asset>();
            // A markup ASSET's painting (since 2026-10-04), and a layer's
            // from before: Save and the working copy's sweep keep them.
            if (a.markup && !a.markup->raster.empty()) {
              held.insert(a.markup->raster.hash.hex());
            }
            for (const auto& l : a.layers) {
              if (l.markup && !l.markup->raster.empty()) {
                held.insert(l.markup->raster.hash.hex());
              }
            }
          }
          return true;
        });
  }));
  return held;
}

// ---- generation history -----------------------------------------------

Status
Project::add_history(const HistoryEntry& h)
{
  return _env->write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _db.history, id_key(h.id), Json(h));
  });
}

Result<std::vector<HistoryEntry>>
Project::history() const
{
  std::vector<HistoryEntry> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    return db::for_each_prefix(txn, _db.history, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            out.push_back(j->get<HistoryEntry>());
          }
          return true;
        });
  }));
  // By when each state was; the ids (UUIDv7) break ties in order made.
  std::ranges::stable_sort(out, {}, &HistoryEntry::created_ms);
  return out;
}

// ---- subjects ---------------------------------------------------------

Result<Subject>
Project::create_subject(std::string name, std::string kind)
{
  Subject s;
  s.id = SubjectId::make();
  s.name = std::move(name);
  s.kind = std::move(kind);
  s.created_ms = now_ms();
  s.modified_ms = s.created_ms;
  VALTZ_TRY(put_subject(s));
  return s;
}

Status
Project::put_subject(const Subject& s)
{
  return _env->write([&](db::Txn& txn) -> Status {
    // Rebuild the reverse index for this subject's references.
    VALTZ_ASSIGN(auto old, get_cbor(txn, _db.subjects, id_key(s.id)));
    if (old) {
      for (const auto& e : old->get<Subject>().entries) {
        VALTZ_TRY(txn.del(_db.subject_refs, id_key(e.asset),
                          id_key(s.id)));
      }
    }
    for (const auto& e : s.entries) {
      VALTZ_ASSIGN(auto a, get_asset_(txn, e.asset));
      if (!a) {
        return make_error(Code::NotFound, std::format(
            "subject entry '{}' refers to a missing asset", e.role));
      }
      VALTZ_TRY(txn.put(_db.subject_refs, id_key(e.asset), id_key(s.id)));
    }
    Subject rec = s;
    rec.modified_ms = now_ms();
    return put_cbor(txn, _db.subjects, id_key(s.id), Json(rec));
  });
}

Result<Subject>
Project::subject(SubjectId id) const
{
  Subject out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(auto j, get_cbor(txn, _db.subjects, id_key(id)));
    if (!j) {
      return make_error(Code::NotFound, "no such subject");
    }
    out = j->get<Subject>();
    return ok_status();
  }));
  return out;
}

Result<std::vector<Subject>>
Project::subjects() const
{
  std::vector<Subject> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    return db::for_each_prefix(txn, _db.subjects, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            out.push_back(j->get<Subject>());
          }
          return true;
        });
  }));
  return out;
}

Result<std::vector<SubjectId>>
Project::subjects_referencing(AssetId id) const
{
  std::vector<SubjectId> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(db::Cursor c, txn.cursor(_db.subject_refs));
    for (auto e = c.seek_exact(id_key(id)); e; e = c.next_dup()) {
      out.push_back(id_from_bytes<SubjectId>(e->value));
    }
    return ok_status();
  }));
  return out;
}

}
