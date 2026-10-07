// The SAVE FORMAT (DESIGN §5b): a project as saved, one CBOR document in
// its package beside the blobs.
//
//   Name.valtz/
//     project.cbor                  every record (below)
//     blobs/<h0h1>/<sha256>.<ext>   content-addressed media
//
//   { "format": "valtz-project", "version": 2, "readable_from": 2,
//     "writer": "valtz 0.1.0", "saved": <ms>,
//     "project": {"id", "name", "created", "schema"},
//     "meta": {<key>: <value>},           folders, ...
//     "tables": {"assets": [...], "versions": [...], "recipes": [...],
//                "subjects": [...], "history": [...],
//                "dependents": [[input, derived], ...],
//                "subject_refs": [[asset, subject], ...], ...},
//     "extensions": [{"id", "version", "name"}],
//     "x": {"<extension id>": ...} }
//
// EXTENSIBLE BY CONSTRUCTION. The records are the CBOR documents the
// working copy's LMDB holds, and each keeps the fields it does not know
// (records.h `rest`); a table, a meta key or a top-level section this
// Valtz does not know is kept and written back. Extension data lives
// under "x": {"<extension id>": ...}, in a record or here at the top. A
// file whose extensions are missing opens whole: what needs them is
// passed over, and is still there at the next save.
//
// VERSIONED as the extension interface is: `version` is the writer's;
// `readable_from`, the oldest reader version that reads all of it. A file
// opens when its `readable_from` is at most kProjectFormat. A format-1
// package (project.lmdb, before this) is a database, not a file: the
// working copy opens it as it is (workspace.h).

#ifndef VALTZ_PROJECT_PROJECT_FILE_H
#define VALTZ_PROJECT_PROJECT_FILE_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <filesystem>
#include <string>

namespace valtz::project {

class Project;

// 3: the document model of DESIGN §6a (records of schema 2); a format-2
// file is migrated as it is read (migrate.h).
inline constexpr int kProjectFormat = 3;
// The oldest format this one's files are fully read by.
inline constexpr int kProjectFormatReadableFrom = 3;
inline constexpr const char* kProjectFile = "project.cbor";
inline constexpr const char* kLegacyDatabase = "project.lmdb";

// The document a save writes: `p`'s records (Project::dump), headed, with
// the extensions whose models or data it holds.
Result<Json> project_document(const Project& p, const Json& extensions);

// `doc` read into `p` (Project::load) -- refused when it is not a project
// file, or one a newer Valtz wrote for readers newer than this one.
Status read_project_document(Project& p, const Json& doc);

// Written atomically: to "<file>.tmp", flushed to the disk, renamed over
// `file`.
Status write_project_file(const std::filesystem::path& file,
                          const Json& doc);
Result<Json> read_project_file(const std::filesystem::path& file);

// A package's saved state, by content: what a working copy was made from.
Result<std::string> project_file_hash(const std::filesystem::path& file);

// The extensions (by id) whose data the document holds: every key under
// an "x" anywhere in its records, and the top level's.
std::vector<std::string> extension_data_in(const Json& doc);

}

#endif
