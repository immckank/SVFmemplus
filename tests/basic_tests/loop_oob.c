// loop_oob.c -- loop-induction out-of-bounds cases for the LLM MAY-triage overlay.
//
// The sound interval analyzer cannot model loop guards (`RangeAnalysis` does not
// process Cmp/Branch), so every loop-induction index below degrades to an
// unbounded range and the access is reported as a MAY -- the sound layer cannot
// tell a real overflow from a safe access. The triage overlay slices each
// surviving MAY (capacity / guard / induction / code snippet) for LLM judgement.
//
// Expected (sound checker): MUST overflow count = 0; several MAYs (>=1).
// The interesting bit is that only SOME of these are *real* overflows -- that is
// exactly what the LLM is asked to disambiguate from the slices.

// --- Case 1: classic off-by-one (linear, guard extractable) ----------------
//   i in 0..10 -> a[10] escapes [0,9].  REAL overflow.
int off_by_one(void)
{
    int a[10];
    int s = 0;
    for (int i = 0; i <= 10; i++)
        s += a[i];
    return s;
}

// --- Case 2: quadratic index a[i*i] (non-linear) ---------------------------
//   Even with i < 10 the index i*i reaches 81, far past [0,9]. The index is a
//   multiply, so neither the interval domain nor the lightweight affine form
//   can bound it.  REAL overflow.
int quadratic_index(void)
{
    int a[10];
    int s = 0;
    for (int i = 0; i < 10; i++)
        s += a[i * i];
    return s;
}

// --- Case 3: nested double loop, flattened 2D index a[i*cols + j] ----------
//   rows=5, cols=5 -> max index 4*5 + 4 = 24, capacity is 20 -> REAL overflow.
int nested_flatten(void)
{
    int g[20];
    int s = 0;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            s += g[i * 5 + j];
    return s;
}

// --- Case 4: triangular nested loop, inner bound depends on outer ----------
//   for i<8: for j<=i: a[j].  j actually stays in [0,7], capacity 8, so this is
//   SAFE, but the analyzer still reports MAY (it cannot prove j <= i < 8).
//   The LLM must reason about the coupled guards to call this safe.
int triangular(void)
{
    int a[8];
    int s = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j <= i; j++)
            s += a[j];
    return s;
}

// --- Case 5: nested loop with product index a[i*j] -------------------------
//   for i<6: for j<6: a[i*j].  max i*j = 5*5 = 25 > capacity 16 -> REAL overflow.
int product_index(void)
{
    int a[16];
    int s = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
            s += a[i * j];
    return s;
}

// --- Case 6: safe simple loop (contrast, should be SAFE) -------------------
//   i in 0..9, capacity 10 -> in bounds.  Reported MAY but truly safe.
int safe_loop(void)
{
    int a[10];
    int s = 0;
    for (int i = 0; i < 10; i++)
        s += a[i];
    return s;
}

int main(void)
{
    return off_by_one() + quadratic_index() + nested_flatten()
         + triangular() + product_index() + safe_loop();
}
