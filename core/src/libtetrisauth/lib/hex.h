#ifndef LIBTETRISAUTH_HEX_H
#define LIBTETRISAUTH_HEX_H

#include <stddef.h>

void hex_encode(const unsigned char *in, size_t n, char *out);

int hex_decode(const char *in, size_t in_len, unsigned char *out, size_t cap);

#endif /* LIBTETRISAUTH_HEX_H */
