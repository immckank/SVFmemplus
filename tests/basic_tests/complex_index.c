// complex_index.c -- complex interprocedural index expressions (k=1 context).
//
// A buffer pointer plus several *scalar* formals are forwarded across the call
// edge; the callee then computes a DERIVED index (product / affine / mixed)
// from those scalar formals. With k=1 call-site binding each formal resolves to
// its precise actual-argument range, so the derived index is recovered exactly
// and a genuine out-of-bounds access is classified as MUST (not merely MAY).
//
// This is the "pass a[] , b , c ; access a[b*c] inside" scenario.
//
//   int g[10]  -> valid index [0, 9]
//   int m[20]  -> valid index [0,19]
//
// Expectations (per call site, k=1 distinguishes them):
//   write_mul   (g, 4, 3) : g[12]      MUST  (12 > 9)
//   write_mul   (g, 2, 5) : g[10]      MUST  (10 > 9)
//   write_mul   (g, 3, 3) : g[9]       safe
//   write_affine(m, 5, 3) : m[5*3+5=20] MUST (20 > 19)
//   write_affine(m, 3, 4) : m[3*4+3=15] safe
//
//   => 3 MUST, 0 MAY

static void write_mul(int* a, int b, int c)
{
    a[b * c] = 1;        // index = b*c, recovered precisely from two formals
}

static void write_affine(int* a, int b, int c)
{
    a[b * c + b] = 2;    // index = b*c + b (Mul then Add over the formals)
}

int complex_index(void)
{
    int g[10];
    write_mul(g, 4, 3);   // g[12] MUST
    write_mul(g, 2, 5);   // g[10] MUST
    write_mul(g, 3, 3);   // g[9]  in bounds, must NOT be reported
    return g[0];
}

int complex_index2(void)
{
    int m[20];
    write_affine(m, 5, 3);  // m[20] MUST
    write_affine(m, 3, 4);  // m[15] in bounds, must NOT be reported
    return m[0];
}

int main(void)
{
    return complex_index() + complex_index2();
}
