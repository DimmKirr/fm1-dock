/* SPDX-License-Identifier: GPL-3.0-only */
/* What the compiler may call in a module (struct copies, clears): linked into every module, so a
 * module links against nothing of the host's. */
#include <stdint.h>

void *memset(void *d, int c, unsigned n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    if (!(((uintptr_t)p | (uintptr_t)q | n) & 3u)) {
        uint32_t *pw = d;
        const uint32_t *qw = s;
        for (n >>= 2; n; n--)
            *pw++ = *qw++;
        return d;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, unsigned n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p <= q || p >= q + n)
        return memcpy(d, s, n);
    while (n--)
        p[n] = q[n];
    return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

unsigned strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}
