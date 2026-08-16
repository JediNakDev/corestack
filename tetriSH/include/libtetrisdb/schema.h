#ifndef LIBTETRISDB_SCHEMA_H
#define LIBTETRISDB_SCHEMA_H

#include <stddef.h>

/* Writes dir's catalog path into dst, truncating to cap. */
void db_catalog_path(char *dst, size_t cap, const char *dir);

/* Creates path and every missing parent, like `mkdir -p`. */
int db_mkdir_p(const char *path);

/* Ensures a table exists, creating it if it does not. */
int db_ensure_table(const char *dir, const char *name, const char *schema);

/* Quotes a string for use as a SQL literal. */
char *db_quote(char *dst, size_t cap, const char *src);

#endif /* LIBTETRISDB_SCHEMA_H */
