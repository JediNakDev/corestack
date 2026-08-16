#include "../system_program.h"

/*
    backup: zip the file or directory named by the BACKUP_DIR
    environment variable and move the archive into
    [PROJECT_DIR]/archive/. The archive filename embeds the datetime
    at which `backup` was run. BACKUP_DIR must be set up by the shell
    (e.g. `setenv BACKUP_DIR=...`) before calling backup.

    Works for both a directory (zipped recursively) and a single file.
*/

int main(void)
{
    /* BACKUP_DIR must be provided by the shell environment. */
    const char *backup_target = getenv("BACKUP_DIR");
    if (backup_target == NULL || backup_target[0] == '\0')
    {
        fprintf(stderr, COLOR_RED "backup: BACKUP_DIR environment variable is "
                                  "not set.\n" COLOR_RESET);
        fprintf(stderr,
                "Set it first, e.g.  setenv BACKUP_DIR=<file-or-directory>\n");
        return EXIT_FAILURE;
    }

    /* The target must actually exist. */
    struct stat st;
    if (stat(backup_target, &st) != 0)
    {
        fprintf(stderr,
                COLOR_RED "backup: cannot access '%s': %s\n" COLOR_RESET,
                backup_target, strerror(errno));
        return EXIT_FAILURE;
    }

    /* [PROJECT_DIR] is wherever the shell launched us from. */
    char project_dir[PATH_MAX];
    if (getcwd(project_dir, sizeof(project_dir)) == NULL)
    {
        perror("getcwd() error");
        return EXIT_FAILURE;
    }

    /* Ensure [PROJECT_DIR]/archive/ exists. */
    char archive_dir[PATH_MAX];
    snprintf(archive_dir, sizeof(archive_dir), "%s/archive", project_dir);
    if (mkdir(archive_dir, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr,
                COLOR_RED "backup: cannot create '%s': %s\n" COLOR_RESET,
                archive_dir, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Build a datetime stamp for the archive name. */
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", lt);

    /* Derive a clean base name from BACKUP_DIR (strip any path and any
       trailing slash) so the archive is named after the target itself. */
    char target_copy[PATH_MAX];
    strncpy(target_copy, backup_target, sizeof(target_copy) - 1);
    target_copy[sizeof(target_copy) - 1] = '\0';
    size_t tlen = strlen(target_copy);
    while (tlen > 1 && target_copy[tlen - 1] == '/')
    {
        target_copy[--tlen] = '\0';
    }
    const char *base = strrchr(target_copy, '/');
    base = (base != NULL) ? base + 1 : target_copy;

    /* Final archive path: [PROJECT_DIR]/archive/<base>_<datetime>.zip */
    char archive_path[PATH_MAX];
    snprintf(archive_path, sizeof(archive_path), "%s/%s_%s.zip", archive_dir,
             base, timestamp);

    /* Build the zip command. `zip -r -q` handles directories; for a
       single file the -r flag is harmless. */
    char command[PATH_MAX * 3];
    snprintf(command, sizeof(command), "zip -r -q '%s' '%s'", archive_path,
             backup_target);

    int rc = system(command);
    if (rc == -1 || WEXITSTATUS(rc) != 0)
    {
        fprintf(stderr,
                COLOR_RED
                "backup: failed to create archive for '%s'\n" COLOR_RESET,
                backup_target);
        return EXIT_FAILURE;
    }

    printf(COLOR_GREEN "Backup successful!\n" COLOR_RESET);
    printf("  Source : %s (%s)\n", backup_target,
           S_ISDIR(st.st_mode) ? "directory" : "file");
    printf("  Archive: %s\n", archive_path);

    return EXIT_SUCCESS;
}
