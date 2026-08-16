#include "libs/perms.h"

// Function to convert permissions to a string
void perms_to_string(mode_t mode, char str[11])
{
    strcpy(str, "----------"); // Default to all permissions off

    if (S_ISDIR(mode))
        str[0] = 'd'; // Directory
    if (S_ISCHR(mode))
        str[0] = 'c'; // Character device
    if (S_ISBLK(mode))
        str[0] = 'b'; // Block device

    if (mode & S_IRUSR)
        str[1] = 'r'; // Owner has read permission
    if (mode & S_IWUSR)
        str[2] = 'w'; // Owner has write permission
    if (mode & S_IXUSR)
        str[3] = 'x'; // Owner has execute permission
    if (mode & S_IRGRP)
        str[4] = 'r'; // Group has read permission
    if (mode & S_IWGRP)
        str[5] = 'w'; // Group has write permission
    if (mode & S_IXGRP)
        str[6] = 'x'; // Group has execute permission
    if (mode & S_IROTH)
        str[7] = 'r'; // Others have read permission
    if (mode & S_IWOTH)
        str[8] = 'w'; // Others have write permission
    if (mode & S_IXOTH)
        str[9] = 'x'; // Others have execute permission
}
