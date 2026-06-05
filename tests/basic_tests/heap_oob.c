// heap_oob.c -- heap buffer out-of-bounds regression cases (byte domain).
//
// Exercises the unified allocation recognition (AllocAPIRegistry):
//   - malloc(n)        : size = arg0
//   - calloc(cnt, sz)  : size = cnt * sz  (ELEM_MUL_ARG)
//   - realloc(p, n)    : size = arg1      (fixes "always arg0" bug)
//
// Expected (MUST overflow): each *_oob store/read past the byte capacity.

#include <stdlib.h>

int malloc_oob(void)
{
    char* p = (char*)malloc(16);
    p[16] = 'x';     // MUST: byte 16 out of [0,15]
    int r = p[0];
    free(p);
    return r;
}

int calloc_oob(void)
{
    int* p = (int*)calloc(4, sizeof(int)); // 16 bytes
    p[4] = 7;        // MUST: byte 16 out of [0,15]
    int r = p[0];
    free(p);
    return r;
}

int realloc_oob(void)
{
    char* p = (char*)malloc(8);
    p = (char*)realloc(p, 4);   // shrink to 4 bytes
    p[7] = 'y';      // MUST: byte 7 out of [0,3] (size taken from arg1)
    int r = p[0];
    free(p);
    return r;
}

int heap_safe(void)
{
    char* p = (char*)malloc(16);
    p[15] = 'z';     // in bounds
    int r = p[15];
    free(p);
    return r;
}

int main(void)
{
    return malloc_oob() + calloc_oob() + realloc_oob() + heap_safe();
}
