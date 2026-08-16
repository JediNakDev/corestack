#include "hex.h"

static const char HEX[] = "0123456789abcdef";

void hex_encode(const unsigned char *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = HEX[in[i] >> 4];       // HHHHLLLL >> 4 -> 0000HHHH
        out[i * 2 + 1] = HEX[in[i] & 0x0f]; // HHHHLLLL & 00001111 -> 0000LLLL
    }
    out[n * 2] = '\0';
}

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int hex_decode(const char *in, size_t in_len, unsigned char *out, size_t cap)
{
    if (in_len % 2 != 0 || in_len / 2 > cap)
        return -1;

    for (size_t i = 0; i < in_len; i += 2)
    {
        int hi = hex_nibble((unsigned char)in[i]);
        int lo = hex_nibble((unsigned char)in[i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        // 0000HHHH << 4 -> HHHH0000; HHHH0000 | 0000LLLL -> HHHHLLLL;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return (int)(in_len / 2);
}
