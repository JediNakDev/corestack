#ifndef TETRISH_TETRISDB_PROVISION_H
#define TETRISH_TETRISDB_PROVISION_H

/* tetriSH's half of the shared tetrisdb daemon's table list.
 *
 * core/src/tetrisdb/main.c is shared between the projects in this repo, so the
 * tables it creates beyond the universal "user" table cannot be hard-coded
 * there. Each project answers with its own copy of this header, found ahead of
 * the shared include path (-Iinclude precedes -I../core/include).
 *
 * tetriSH adds "history": bin/session writes a row per finished round and
 * bin/tetrisctl reads them back, and neither creates the table, so it has to
 * exist before either runs. The name and the schema text come from
 * include/tetrisd/history.h - the same two macros the readers and the writer
 * use - rather than being restated here, so there is one copy to keep in step
 * with the columns history.c actually selects. */

#include "tetrisd/history.h"

#define DB_PROVISION_TABLES {HISTORY_DB_TABLE, HISTORY_DB_SCHEMA},

#endif /* TETRISH_TETRISDB_PROVISION_H */
