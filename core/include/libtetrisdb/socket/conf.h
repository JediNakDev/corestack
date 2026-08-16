#ifndef LIBTETRISDB_SOCKET_CONF_H
#define LIBTETRISDB_SOCKET_CONF_H

#include <stddef.h>
#include <sys/un.h>

/** db_ipc default: SocketRunner's unix socket. Relative, resolved by the
 * caller against the project root, because only the caller knows its root. */
#define DB_DEFAULT_IPC "var/run/tetrisdb.sock"
#define DB_IPC_MAX (sizeof(((struct sockaddr_un *)0)->sun_path) - 1)

#define DB_DEFAULT_TIMEOUT_MS 2000
#define DB_TIMEOUT_MIN_MS 100
#define DB_TIMEOUT_MAX_MS 60000
#define DB_DEFAULT_DIR "var/db"               /**< db_dir default. */
#define DB_DEFAULT_JAR "db/dist/simpledb.jar" /**< db_jar default. */
#define DB_DEFAULT_JAVA "java"                /**< db_java default. */
#define DB_DEFAULT_ERR_PATH "var/log/tetrisdb.err"
#define DB_DEFAULT_SESSIONS 16

#define DB_RUNNER_DEFAULT_WAIT_MS 10000
#endif /* LIBTETRISDB_SOCKET_CONF_H */
