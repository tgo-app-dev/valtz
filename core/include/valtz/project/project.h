// A Valtz project: one macOS package directory holding one LMDB file.
//
//   Name.valtz/
//     project.lmdb        records (LMDB, MDB_NOSUBDIR)
//     project.lmdb-lock
//     blobs/              content-addressed media (blob-store.h)
//     tmp/                build outputs before adoption (same volume as
//                         blobs/, so adoption is a rename)
//
// A package holds only durable data. Disposable derivatives
// (thumbnails, proxies) live in the app-wide managed cache, keyed by
// content (cache/cache-store.h).
//
// Sub-databases (keys are raw 16-byte ids unless noted):
//
//   meta          string key -> CBOR       schema, id, name, created
//   assets        AssetId -> CBOR(Asset)
//   versions      AssetId|be32(n) -> CBOR(AssetVersion)
//   recipes       RecipeId -> CBOR(Recipe)
//   dependents    AssetId -> AssetId       DUPSORT: input -> derived
//   subjects      SubjectId -> CBOR(Subject)
//   subject_refs  AssetId -> SubjectId     DUPSORT: reverse index
//   history       HistoryId -> CBOR(HistoryEntry)
//   x_tables      name -> CBOR array      a saved file's tables this Valtz
//                                         does not know, kept for its save
//   undo          be64(seq) -> CBOR       the undo history (undo.h); never
//                                         in a saved file
//
// A Project is safe to use from several threads: reads run in LMDB
// snapshots, writes serialize on LMDB's single writer.
//
// LMDB files must live on a local volume. A project on iCloud Drive, a
// network share or a Dropbox folder can be corrupted by the sync client
// rewriting the memory-mapped file underneath us; open() refuses such
// paths rather than risk it.

#ifndef VALTZ_PROJECT_PROJECT_H
#define VALTZ_PROJECT_PROJECT_H

#include "valtz/base/result.h"
#include "valtz/db/lmdb.h"
#include "valtz/project/blob-store.h"
#include "valtz/project/records.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace valtz::project {

inline constexpr const char* kPackageExt = ".valtz";

// Where an imported file's bytes live.
enum class Placement : std::uint8_t {
  // Video is LINKED in place: camera originals are too large to
  // duplicate and users keep them organized themselves. Everything else
  // is COPIED into the package (an instant APFS clone on the same
  // volume).
  Auto,
  Copy,
  Link,
};

bool links_in_place(Placement, media::MediaType);

struct ImportOptions {
  Placement   placement = Placement::Auto;
  std::string name;  // defaults to the file name
};

enum class LinkState : std::uint8_t {
  Ok,         // where we left it, unchanged
  Relocated,  // moved or renamed; found through its bookmark
  Modified,   // present, but its size or mtime changed since import
  Missing,    // gone, or its volume is not mounted
};

const char* to_str(LinkState);

struct LinkCheck {
  LinkState             state = LinkState::Missing;
  std::filesystem::path path;  // where it is now (empty when missing)
};

enum class StaleReason : std::uint8_t {
  Fresh,
  NeverBuilt,
  RecipeChanged,
  InputChanged,
  InputStale,    // an upstream derived input is itself stale
  InputMissing,  // an input asset was deleted or has no version
};

const char* to_str(StaleReason);

struct Staleness {
  StaleReason reason = StaleReason::Fresh;
  AssetId     culprit;  // the input responsible, when there is one
  bool fresh() const noexcept { return reason == StaleReason::Fresh; }
};

struct BuildStep {
  AssetId     asset;
  RecipeId    recipe;
  StaleReason reason;
  bool        needs_confirmation;  // non-deterministic recipe
};

class Project {
public:
  static Result<std::unique_ptr<Project>>
  create(const std::filesystem::path& package, std::string name);
  static Result<std::unique_ptr<Project>>
  open(const std::filesystem::path& package);

  ~Project();

  ProjectId id() const noexcept { return _id; }
  const std::string& name() const noexcept { return _name; }
  // The project's own name, in its meta.
  Status set_name(std::string name);
  const std::filesystem::path& package() const noexcept { return _pkg; }
  BlobStore& blobs() noexcept { return _blobs; }
  const BlobStore& blobs() const noexcept { return _blobs; }

  // ---- assets -------------------------------------------------------

  // Probe, store and record a user-provided file as a new source asset.
  // Blocking (hashes the file); call off the main thread.
  Result<Asset> import_file(const std::filesystem::path& src,
                            const ImportOptions& opts = {});
  // `tags`: what it is ("prompt", a captured prompt).
  Result<Asset> add_text(std::string name, std::string_view text,
                         std::vector<std::string> tags = {});
  // Replace a source asset's content with a new version (e.g. the user
  // edits a description). Dependents become stale.
  Result<AssetVersion> update_text(AssetId, std::string_view text);

  Result<Asset> asset(AssetId) const;
  Result<std::vector<Asset>> assets() const;
  // `number` 0 = head.
  Result<AssetVersion> version(AssetId, std::uint32_t number = 0) const;
  Result<std::vector<AssetVersion>> versions(AssetId) const;
  Status rename_asset(AssetId, std::string name);
  // Replace the asset's modifiers (Modifier): no new version -- the bytes
  // are unchanged -- and nothing downstream goes stale (a recipe records
  // what it used).
  Status set_modifiers(AssetId, std::vector<Modifier>);
  Status set_layers(AssetId, std::vector<Layer>);
  // Any of an asset's fields changed in one write: `edit` gets the record
  // as stored and changes it (its id, kind, versions and recipe are kept).
  Status update_asset(AssetId, const std::function<Status(Asset&)>& edit);
  // A new asset that no recipe makes -- a composition, a markup, a flat
  // copy -- recorded as `a` says (a fresh id; its name made unique as
  // define_derived's is). Versions are added apart (share_version).
  Result<Asset> add_asset(Asset a, std::size_t max_name_bytes = 0);
  // The PROJECT'S composition (DESIGN §6a): what the stage shows of the
  // work. None until set.
  Result<std::optional<AssetId>> composition() const;
  Status set_composition(std::optional<AssetId>);
  // The schema the records are in (meta "project".schema).
  Result<std::uint32_t> schema() const;
  // A schema-1 project read in was brought over to this one (migrate.h).
  bool migrated() const noexcept { return _migrated; }
  Status set_canvas(AssetId, const media::StackCanvas&);
  Status set_timeline(AssetId, std::int64_t frames);
  // The asset list's FOLDERS ({"id", "name"} each, in order), kept in
  // the project's meta; an asset names its own (Asset::folder).
  Result<Json> folders() const;
  Status set_folders(const Json& folders);
  // How the project was last LOOKED AT -- an app's own state for it (its
  // window's size: {"window": {"width", "height"}}), kept in its meta and
  // saved with it; {} when none.
  Result<Json> view_state() const;
  Status set_view_state(const Json& view);
  // The project's OUTPUT (records.h OutputSettings), in its meta; the
  // defaults when it was never set.
  Result<OutputSettings> output() const;
  Status set_output(const OutputSettings&);
  Status set_asset_folder(AssetId, std::string folder);
  // A version of `dst` that IS `src`'s version `number` -- the same
  // blob, no copy -- built by `recipe` from `inputs`: an asset that
  // starts as another and records its own look over it (a modified
  // picture, a capture).
  Result<AssetVersion> share_version(AssetId dst, AssetId src,
                                     std::uint32_t number,
                                     const Recipe& recipe,
                                     const std::vector<ResolvedInput>& in);
  // The asset gone from the project: its record, its versions, its
  // recipe's edges -- and the blobs no other version or history state
  // holds. Refused while another asset is built from it.
  Status remove_asset(AssetId);

  // Where a version's bytes are: the blob, or the linked original. A
  // linked original is verified first -- relocations are followed; a
  // modified or missing original is an error until refresh_link() runs,
  // because its bytes no longer match the recorded content hash.
  Result<std::filesystem::path> media_path(AssetId,
                                           std::uint32_t number = 0) const;

  // Where a linked source is now, without changing anything.
  Result<LinkCheck> check_link(AssetId) const;
  // Bring a linked source up to date: record a relocation; if its
  // content changed, re-probe and re-hash it and append a NEW VERSION,
  // so dependents go stale by content exactly as for any other edit.
  // Blocking (may hash a large file).
  Result<LinkCheck> refresh_link(AssetId);
  Result<std::string> read_text(AssetId, std::uint32_t number = 0) const;

  // ---- derived assets and recipes -----------------------------------

  // Declare a derived asset and the recipe that makes it. No version
  // exists until a build commits one; until then it is NeverBuilt.
  //
  // The name is made unique in the project -- the same prompt is often
  // run again -- as "name", "name (2)", "name (3)"... With
  // `max_name_bytes`, it is also cut to fit, suffix included, at a full
  // character and marked with "…" (base/text.h). The returned Asset
  // carries the name as stored. Chosen inside the write transaction, so
  // two definitions at once cannot take the same name.
  Result<Asset> define_derived(std::string name, AssetKind, Recipe,
                               std::size_t max_name_bytes = 0);
  // Point a derived asset at a new (immutable) recipe.
  Status set_recipe(AssetId, Recipe);
  Result<Recipe> recipe(RecipeId) const;

  // Resolve a recipe's inputs to exact versions and content hashes.
  Result<std::vector<ResolvedInput>> resolve_inputs(const Recipe&) const;

  // Record a finished build of `asset`: adopt `output` (a file under
  // blobs().tmp_dir()) and append a version carrying the snapshot.
  struct BuildRecord {
    Recipe                     recipe;    // as executed
    std::vector<ResolvedInput> inputs;    // as resolved at build start
    std::filesystem::path      output;
    media::MediaInfo           info;
    std::string                engine;
    std::string                host;
    std::string                model_digest;
    Json                       timing = Json::object();  // AssetVersion's
    Json                       outputs = Json::object();  // AssetVersion's
  };
  Result<AssetVersion> commit_build(AssetId, BuildRecord);

  // ---- the dependency graph (project-graph.cc) -----------------------

  Result<Staleness> staleness(AssetId) const;
  // What an asset IS, as content: a version's bytes for a flat or
  // generated asset (`version` 0: its head); for a markup its pixels and
  // objects; for a composition its structure -- layers, looks, time,
  // transitions -- with what each layer shows, recursively. Equal keys
  // draw equal pictures: a composition's RENDER KEY (DESIGN §6a), and what
  // a recipe records of an input that is one.
  Result<ContentHash> content_key(AssetId, std::uint32_t version = 0) const;
  // Direct (or all transitive) derived assets that consume `asset`.
  Result<std::vector<AssetId>> dependents(AssetId, bool transitive) const;
  // Everything that must be rebuilt, in dependency order, to bring
  // `targets` up to date. Fresh assets are omitted.
  Result<std::vector<BuildStep>>
  plan_build(const std::vector<AssetId>& targets) const;

  // ---- generation history -----------------------------------------
  // States kept as they were (HistoryEntry), oldest first.

  Status add_history(const HistoryEntry&);
  Result<std::vector<HistoryEntry>> history() const;

  // ---- the save format (project-file.h) ----------------------------

  // Every record, by table, as the save format holds it: {"meta": {key:
  // value}, "tables": {"assets": [...], ..., "dependents": [[input,
  // derived], ...], and the x_tables kept}}. The undo history is not in
  // it.
  Result<Json> dump() const;
  // Every record replaced by `doc` (dump()'s shape) in one transaction,
  // unjournaled: a saved file read in, or read back (Revert). A table it
  // does not know is kept in x_tables. The project's id and name are
  // read from its meta.
  Status load(const Json& doc);
  // The blobs the records hold: versions', history states', markup
  // rasters' (by hash).
  Result<std::set<std::string>> held_blobs() const;
  // The undo table's handle, and the journaled tables' (undo.h).
  db::Env& env() noexcept { return *_env; }
  db::Dbi undo_table() const noexcept { return _db.undo; }
  std::vector<db::Dbi> record_tables() const;
  // A record table's name by its handle ("" none), and back.
  std::string table_name(unsigned handle) const;
  std::optional<db::Dbi> table(std::string_view name) const;
  // Removing an asset leaves its blobs (the working copy collects them,
  // keeping what the undo history holds).
  void keep_blobs(bool keep) noexcept { _keep_blobs = keep; }

  // ---- subjects -----------------------------------------------------

  Result<Subject> create_subject(std::string name, std::string kind);
  Status put_subject(const Subject&);
  Result<Subject> subject(SubjectId) const;
  Result<std::vector<Subject>> subjects() const;
  Result<std::vector<SubjectId>> subjects_referencing(AssetId) const;

private:
  Project() = default;
  Status open_dbs_();
  Result<std::uint32_t> append_version_(db::Txn&, Asset&, AssetVersion&);
  Result<std::optional<Asset>> get_asset_(const db::Txn&, AssetId) const;
  Result<std::string> unique_name_(const db::Txn&, std::string_view name,
                                   std::size_t max_bytes) const;
  Result<std::optional<AssetVersion>>
  get_version_(const db::Txn&, AssetId, std::uint32_t) const;
  Result<std::optional<Recipe>> get_recipe_(const db::Txn&, RecipeId) const;
  Result<std::vector<ResolvedInput>>
  resolve_inputs_(const db::Txn&, const Recipe&) const;
  Result<Staleness> staleness_(const db::Txn&, AssetId, int depth) const;
  Result<ContentHash> content_key_(const db::Txn&, AssetId,
                                   std::uint32_t version, int depth) const;

  std::unique_ptr<db::Env> _env;
  std::filesystem::path    _pkg;
  BlobStore                _blobs{""};
  ProjectId                _id;
  std::string              _name;
  bool                     _keep_blobs = false;
  bool                     _migrated = false;

  struct Dbis {
    db::Dbi meta, assets, versions, recipes, dependents, subjects,
        subject_refs, history, x_tables, undo;
  } _db;
};

// Is `path` on a volume LMDB can safely memory-map (local, not synced)?
Status check_local_volume(const std::filesystem::path&);

}

#endif
