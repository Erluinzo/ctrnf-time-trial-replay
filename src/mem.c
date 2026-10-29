/* The compiler may turn struct copies and zero-filled arrays into calls to these, even in
 * freestanding code. Built with -fno-tree-loop-distribute-patterns so they do not call themselves. */
#include <stddef.h>

void *memcpy(void *to, const void *from, size_t n)
{
    char *t = to;
    const char *f = from;
    while (n--) *t++ = *f++;
    return to;
}

void *memmove(void *to, const void *from, size_t n)
{
    char *t = to;
    const char *f = from;
    if (t < f)
        while (n--) *t++ = *f++;
    else
        while (n--) t[n] = f[n];
    return to;
}

void *memset(void *to, int c, size_t n)
{
    char *t = to;
    while (n--) *t++ = (char)c;
    return to;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
