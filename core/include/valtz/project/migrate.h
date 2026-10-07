// A project from before the document model of DESIGN §6a (schema 1),
// brought over to it (schema 2) as it is read: the save format's document
// (Project::dump's shape) in, the same document of the new schema out.
//
// It runs once, on the document, before a record is written -- a pure
// function of what was saved, so a test can hold it to what it does:
//   * every asset gets its CLASS: imports flat, generations generated,
//     the "project", "capture" and "modify" assets compositions (a still
//     for pictures); a layer that showed the asset's own image shows the
//     asset its recipe names (the capture's "own", the copy's "base");
//   * a flat or generated asset that carried a LOOK (modifiers, a stack, a
//     canvas, a timeline) keeps its file, and a new composition ("<name>,
//     edited") takes the look, its layer 0 showing it;
//   * a clip's "trim" becomes its layer's marks;
//   * every markup layer's content becomes a MARKUP asset that it shows;
//   * the PROJECT's composition is the newest "project" asset by change;
//   * a clip stack sounded only its bottom clip: its other clips get
//     volume 0, so it sounds as it did.

#ifndef VALTZ_PROJECT_MIGRATE_H
#define VALTZ_PROJECT_MIGRATE_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

namespace valtz::project {

// The schema a document's records are in (meta "project".schema; 1 when
// unset).
std::uint32_t document_schema(const Json& doc);

// `doc` brought over to kSchemaVersion; unchanged when it is there.
Status migrate_document(Json& doc);

}

#endif
