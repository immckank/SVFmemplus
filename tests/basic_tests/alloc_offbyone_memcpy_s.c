// alloc_offbyone_memcpy_s.c -- symbolic under-allocation regression cases.
//
// Exercises the symbolic affine relation check in checkMemoryOps /
// trySymbolicUnderAlloc: the allocation size and the copy length share the
// SAME symbolic base (`len`), so even though `len` is numerically unknown
// (TOP) the analyzer can still prove "allocated < copied" by comparing the
// integer offsets after the base cancels. This mirrors the OpenHarmony
// BO-4 / BO-5 defects (malloc/OsalMemCalloc(len-1) then memcpy_s(buf,len,..)).
//
// Expected (MUST overflow):
//   - const_under:  malloc(len-2) then memcpy_s(buf,len,..,len)   (every path overflows)
// Expected (MAY overflow):
//   - cond_under:   malloc(len-1 on a branch) then memcpy_s(buf,len,..,len)
// Expected (in bounds, no report):
//   - exact_alloc:  malloc(len)   then memcpy_s(buf,len,..,len)

#include <stdlib.h>

// errno_t memcpy_s(void *dest, size_t destMax, const void *src, size_t count);
// Recognised by the BOF MemCopyAPIRegistry (copy length == arg 3 / count).
extern int memcpy_s(void *dest, unsigned destMax, const void *src, unsigned count);

// Conditional off-by-one: under-allocate by 1 only on a value-dependent branch.
// Allocation size base == copy length base == `len`  ->  MAY overflow.
int cond_under(unsigned len, const char *src)
{
    unsigned allocLen = len;
    if (((len & 0x20u) != 0) && len > 1)
        allocLen = len - 1;            // 1 byte short on this branch
    char *buf = (char *)malloc(allocLen);
    if (buf == NULL)
        return 0;
    memcpy_s(buf, len, src, len);      // MAY: copies len into a possibly len-1 buffer
    return buf[0];
}

// Unconditional under-allocation: every path is short by 2 bytes -> MUST overflow.
int const_under(unsigned len, const char *src)
{
    char *buf = (char *)malloc(len - 2);
    if (buf == NULL)
        return 0;
    memcpy_s(buf, len, src, len);      // MUST: copies len into a len-2 buffer
    return buf[0];
}

// Exact allocation: allocated == copied (same base, offset 0) -> no report.
int exact_alloc(unsigned len, const char *src)
{
    char *buf = (char *)malloc(len);
    if (buf == NULL)
        return 0;
    memcpy_s(buf, len, src, len);      // in bounds
    return buf[0];
}

int main(void)
{
    char src[8] = {0};
    return cond_under(7, src) + const_under(7, src) + exact_alloc(7, src);
}
