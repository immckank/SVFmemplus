// memcpy_oob.c -- memory-copy / string API overflow regression cases.
//
// checkMemoryOps + MemCopyAPIRegistry. All buffers are char (byte-precise).
//
// Expected (MUST overflow):
//   - memcpy_oob:  memcpy(dst[8], src, 16)   -> writes 16 bytes into 8
//   - memset_oob:  memset(buf[8], 0, 16)     -> writes 16 bytes into 8
//   - strncpy_oob: strncpy(dst[4], src, 16)  -> writes up to 16 into 4
// Expected (in bounds, no report):
//   - copy_safe:   memcpy(dst[16], src, 16)

#include <string.h>
#include <stdlib.h>

int memcpy_oob(void)
{
    char dst[8];
    char src[16];
    memcpy(dst, src, 16);   // MUST: 16 bytes into 8-byte dst
    return dst[0];
}

int memset_oob(void)
{
    char buf[8];
    memset(buf, 0, 16);     // MUST: 16 bytes into 8-byte buf
    return buf[0];
}

int strncpy_oob(void)
{
    char dst[4];
    char src[16];
    strncpy(dst, src, 16);  // MUST: up to 16 bytes into 4-byte dst
    return dst[0];
}

int copy_safe(void)
{
    char dst[16];
    char src[16];
    memcpy(dst, src, 16);   // in bounds
    return dst[0];
}

int main(void)
{
    return memcpy_oob() + memset_oob() + strncpy_oob() + copy_safe();
}
