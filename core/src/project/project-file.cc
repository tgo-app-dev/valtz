#include "valtz/project/project-file.h"

#include "valtz/project/migrate.h"

#include "valtz/base/hash.h"
#include "valtz/project/project.h"
#include "project/keys.h"

#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <fstream>
#include <iterator>
#include <set>

namespace valtz::project {

namespace fs = std::filesystem;

namespace {

// The top-level keys this writer sets; anything else in a file is kept.
constexpr std::initializer_list<std::string_view> kHeadKeys = {
    "format", "version", "readable_from", "writer", "saved",
    "project", "meta", "tables", "extensions"};

// Meta keys the working copy keeps for the file, never written as meta.
constexpr const char* kFileRest = "file.rest";
constexpr const char* kFileExtensions = "file.extensions";

template <class IdT>
std::optional<IdT>
id_of(const Json& doc, const char* key)
{
  const IdT id = jget(doc, key, IdT{});
  return id.is_nil() ? std::nullopt : std::optional<IdT>(id);
}

template <class IdT>
std::optional<IdT>
id_at(const Json& pair, std::size_t i)
{
  if (!pair.is_array() || pair.size() <= i || !pair[i].is_string()) {
    return std::nullopt;
  }
  return IdT::parse(pair[i].get<std::string>());
}

void
collect_x(const Json& j, std::set<std::string>& out)
{
  if (j.is_object()) {
    for (const auto& [k, v] : j.items()) {
      if (k == "x" && v.is_object()) {
        for (const auto& [id, _] : v.items()) {
          out.insert(id);
        }
      }
      collect_x(v, out);
    }
  } else if (j.is_array()) {
    for (const auto& v : j) {
      collect_x(v, out);
    }
  }
}

}

// ---- Project's records, whole ------------------------------------------

Result<Json>
Project::dump() const
{
  Json meta = Json::object();
  Json tables = Json::object();
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_TRY(db::for_each_prefix(txn, _db.meta, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            meta[std::string(db::as_view(e.key))] = std::move(*j);
          }
          return true;
        }));
    auto docs = [&](db::Dbi d) -> Result<Json> {
      Json out = Json::array();
      VALTZ_TRY(db::for_each_prefix(txn, d, {},
          [&](const db::Cursor::Entry& e) {
            if (auto j = from_cbor(e.value); j.ok()) {
              out.push_back(std::move(*j));
            }
            return true;
          }));
      return out;
    };
    VALTZ_ASSIGN(tables["assets"], docs(_db.assets));
    VALTZ_ASSIGN(tables["versions"], docs(_db.versions));
    VALTZ_ASSIGN(tables["recipes"], docs(_db.recipes));
    VALTZ_ASSIGN(tables["subjects"], docs(_db.subjects));
    VALTZ_ASSIGN(tables["history"], docs(_db.history));
    auto pairs = [&](db::Dbi d, auto key_tag, auto value_tag)
        -> Result<Json> {
      using K = decltype(key_tag);
      using V = decltype(value_tag);
      Json out = Json::array();
      VALTZ_TRY(db::for_each_prefix(txn, d, {},
          [&](const db::Cursor::Entry& e) {
            out.push_back(Json::array({id_from_bytes<K>(e.key).str(),
                                       id_from_bytes<V>(e.value).str()}));
            return true;
          }));
      return out;
    };
    VALTZ_ASSIGN(tables["dependents"],
                 pairs(_db.dependents, AssetId{}, AssetId{}));
    VALTZ_ASSIGN(tables["subject_refs"],
                 pairs(_db.subject_refs, AssetId{}, SubjectId{}));
    // Tables a file had that this Valtz does not know: as they were.
    return db::for_each_prefix(txn, _db.x_tables, {},
        [&](const db::Cursor::Entry& e) {
          if (auto j = from_cbor(e.value); j.ok()) {
            tables[std::string(db::as_view(e.key))] = std::move(*j);
          }
          return true;
        });
  }));
  return Json{{"meta", std::move(meta)}, {"tables", std::move(tables)}};
}

Status
Project::load(const Json& doc_in)
{
  // A project from before the document model: brought over as it is read.
  Json migrated;
  const Json* src = &doc_in;
  _migrated = false;
  if (document_schema(doc_in) < kSchemaVersion) {
    migrated = doc_in;
    VALTZ_TRY(migrate_document(migrated));
    src = &migrated;
    _migrated = true;
  }
  const Json& doc = *src;
  const Json meta = jget(doc, "meta", Json::object());
  const Json tables = jget(doc, "tables", Json::object());
  // Read in whole: not a change anyone undoes.
  db::Env::Unjournaled quiet;
  VALTZ_TRY(_env->write([&](db::Txn& txn) -> Status {
    for (const db::Dbi d : record_tables()) {
      VALTZ_TRY(txn.clear(d));
    }
    for (const auto& [k, v] : meta.items()) {
      VALTZ_TRY(put_cbor(txn, _db.meta, db::as_bytes(k), v));
    }
    auto list = [&](const char* name) {
      const Json l = jget(tables, name, Json::array());
      return l.is_array() ? l : Json::array();
    };
    for (const auto& d : list("assets")) {
      if (auto id = id_of<AssetId>(d, "id")) {
        VALTZ_TRY(put_cbor(txn, _db.assets, id_key(*id), d));
      }
    }
    for (const auto& d : list("versions")) {
      if (auto id = id_of<AssetId>(d, "asset")) {
        const auto k = version_key(*id, jget<std::uint32_t>(d, "number", 0));
        VALTZ_TRY(put_cbor(txn, _db.versions, {k.data(), k.size()}, d));
      }
    }
    for (const auto& d : list("recipes")) {
      if (auto id = id_of<RecipeId>(d, "id")) {
        VALTZ_TRY(put_cbor(txn, _db.recipes, id_key(*id), d));
      }
    }
    for (const auto& d : list("subjects")) {
      if (auto id = id_of<SubjectId>(d, "id")) {
        VALTZ_TRY(put_cbor(txn, _db.subjects, id_key(*id), d));
      }
    }
    for (const auto& d : list("history")) {
      if (auto id = id_of<HistoryId>(d, "id")) {
        VALTZ_TRY(put_cbor(txn, _db.history, id_key(*id), d));
      }
    }
    for (const auto& p : list("dependents")) {
      auto a = id_at<AssetId>(p, 0);
      auto b = id_at<AssetId>(p, 1);
      if (a && b) {
        VALTZ_TRY(txn.put(_db.dependents, id_key(*a), id_key(*b)));
      }
    }
    for (const auto& p : list("subject_refs")) {
      auto a = id_at<AssetId>(p, 0);
      auto b = id_at<SubjectId>(p, 1);
      if (a && b) {
        VALTZ_TRY(txn.put(_db.subject_refs, id_key(*a), id_key(*b)));
      }
    }
    // A table this Valtz does not know: kept whole for the next save.
    static const std::set<std::string> known = {
        "assets", "versions", "recipes", "subjects", "history",
        "dependents", "subject_refs"};
    for (const auto& [name, t] : tables.items()) {
      if (!known.contains(name)) {
        VALTZ_TRY(put_cbor(txn, _db.x_tables, db::as_bytes(name), t));
      }
    }
    return ok_status();
  }));
  const Json head = jget(meta, "project", Json::object());
  _id = jget(head, "id", _id);
  _name = jget<std::string>(head, "name", _name);
  return ok_status();
}

// ---- the file ---------------------------------------------------------

Result<Json>
project_document(const Project& p, const Json& extensions)
{
  VALTZ_ASSIGN(Json d, p.dump());
  Json meta = jget(d, "meta", Json::object());
  Json doc = jget(meta, kFileRest, Json::object());
  if (!doc.is_object()) {
    doc = Json::object();
  }
  // The extensions it holds data of: those of the file it was read from,
  // and those named now.
  Json uses = jget(meta, kFileExtensions, Json::array());
  if (!uses.is_array()) {
    uses = Json::array();
  }
  for (const auto& e : extensions.is_array() ? extensions : Json::array()) {
    const auto id = jget<std::string>(e, "id", "");
    auto same = std::ranges::find_if(uses, [&](const Json& u) {
      return jget<std::string>(u, "id", "") == id;
    });
    if (same == uses.end()) {
      uses.push_back(e);
    } else {
      *same = e;  // this Valtz's version of it
    }
  }
  const Json head = jget(meta, "project", Json::object());
  meta.erase("project");
  meta.erase(kFileRest);
  meta.erase(kFileExtensions);
  doc["format"] = "valtz-project";
  doc["version"] = kProjectFormat;
  doc["readable_from"] = kProjectFormatReadableFrom;
  doc["writer"] = std::string("valtz ") + VALTZ_VERSION;
  doc["saved"] = now_ms();
  doc["project"] = head;
  doc["meta"] = std::move(meta);
  doc["tables"] = jget(d, "tables", Json::object());
  doc["extensions"] = std::move(uses);
  return doc;
}

Status
read_project_document(Project& p, const Json& doc)
{
  if (!doc.is_object() ||
      jget<std::string>(doc, "format", "") != "valtz-project") {
    return make_error(Code::InvalidArgument, "not a Valtz project file");
  }
  const int readable_from = jget(doc, "readable_from",
                                 jget(doc, "version", 0));
  if (readable_from > kProjectFormat) {
    return make_error(Code::Unsupported, std::format(
        "this project was saved by a newer Valtz (format {}; this one "
        "reads up to {})", readable_from, kProjectFormat));
  }
  Json meta = jget(doc, "meta", Json::object());
  if (!meta.is_object()) {
    meta = Json::object();
  }
  meta["project"] = jget(doc, "project", Json::object());
  meta[kFileRest] = rest_of(doc, kHeadKeys);
  meta[kFileExtensions] = jget(doc, "extensions", Json::array());
  return p.load({{"meta", std::move(meta)},
                 {"tables", jget(doc, "tables", Json::object())}});
}

Status
write_project_file(const fs::path& file, const Json& doc)
{
  const auto bytes = to_cbor(doc);
  const fs::path tmp = file.string() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            tmp.string()));
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n <= 0) {
      ::close(fd);
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
    done += static_cast<std::size_t>(n);
  }
  // Down to the disk before it replaces the old save.
  (void)::fcntl(fd, F_FULLFSYNC);
  ::close(fd);
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot save {}: {}",
                                            file.string(), ec.message()));
  }
  return ok_status();
}

Result<Json>
read_project_file(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return make_error(Code::NotFound, std::format("cannot read {}",
                                                  file.string()));
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  return from_cbor(db::as_bytes(bytes));
}

Result<std::string>
project_file_hash(const fs::path& file)
{
  VALTZ_ASSIGN(ContentHash h, hash_file(file));
  return h.hex();
}

std::vector<std::string>
extension_data_in(const Json& doc)
{
  std::set<std::string> ids;
  collect_x(doc, ids);
  return {ids.begin(), ids.end()};
}

}
