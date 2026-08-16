/*
 * playername.c - the player-name rule. Contract in libtetrisutil/name.h.
 *
 * No ctype.h anywhere in here on purpose: isalnum() is locale-sensitive, and
 * the client's form validator used it, so a name the client accepted was not
 * always a name the server would.
 */

#include "libtetrisutil/name.h"

bool user_name_char_ok(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool user_name_ok(const char *s, size_t len)
{
    if (s == NULL || len < 1 || len > USER_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        if (!user_name_char_ok((unsigned char)s[i]))
            return false;
    }
    return true;
}

int user_name_fold(char *dst, size_t cap, const char *s, size_t len)
{
    if (dst == NULL || cap < MAX_USER_NAME || !user_name_ok(s, len))
        return -1;

    for (size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)s[i];
        dst[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : (char)ch;
    }
    dst[len] = '\0';
    return 0;
}
