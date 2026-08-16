#ifndef BALLOTBOX_TETRISDB_PROVISION_H
#define BALLOTBOX_TETRISDB_PROVISION_H

/* ballotbox's half of the shared tetrisdb daemon's table list.
 *
 * core/src/tetrisdb/main.c is shared between the projects in this repo, so the
 * tables it creates beyond the universal "user" table cannot be hard-coded
 * there. Each project answers with its own copy of this header, found ahead of
 * the shared include path (-Iinclude precedes -I../core/include).
 *
 * ballotbox adds none. Its six tables are ensured by bin/ballotd at startup
 * instead (src/ballotd/main.c, against the BB_DB_TABLE_ and BB_DB_SCHEMA_
 * definitions in include/libballotbrain/db.h), so listing them here as well
 * would be a second copy of the same schema text that nothing keeps in step
 * with the first. */

#define DB_PROVISION_TABLES

#endif /* BALLOTBOX_TETRISDB_PROVISION_H */
