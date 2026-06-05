// complex_heap.c -- computed multiplicative index into a HEAP buffer forwarded
// across a call (k=1 context). Exercises the byte-domain offset scaling on top
// of the "pass a[] , b , c ; access a[b*c]" pattern.
//
//   p = malloc(10 * sizeof(int))  -> 40 bytes -> valid int index [0, 9]
//   q = calloc(8, sizeof(int))    -> 32 bytes -> valid int index [0, 7]
//
// Expectations (per call site, k=1 distinguishes them):
//   store_at(p, 4, 4) : p[16]   MUST  (byte 64 > 39)
//   store_at(p, 3, 3) : p[9]    safe
//   store_at(q, 3, 3) : q[9]    MUST  (byte 36 > 31)
//   store_at(q, 2, 3) : q[6]    safe
//
//   => 2 MUST, 0 MAY

#include <stdlib.h>

static void store_at(int* a, int b, int c)
{
    a[b * c] = 0;        // heap index = b*c, scaled to bytes internally
}

int complex_heap(void)
{
    int* p = (int*)malloc(10 * sizeof(int));
    store_at(p, 4, 4);   // p[16] MUST
    store_at(p, 3, 3);   // p[9]  in bounds, must NOT be reported
    free(p);

    int* q = (int*)calloc(8, sizeof(int));
    store_at(q, 3, 3);   // q[9]  MUST
    store_at(q, 2, 3);   // q[6]  in bounds, must NOT be reported
    free(q);
    return 0;
}

int main(void)
{
    return complex_heap();
}
