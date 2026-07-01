// mask_idx.c -- masked-index precision (TODO-3 regression).
//
// A bitwise mask bounds an index regardless of the (unknown) masked value:
//   i = x & 0xF  =>  i in [0,15] for ANY x.
// Previously an unresolved x (BOTTOM) made bit_and collapse to BOTTOM and the
// index degraded to TOP at handleGep -> spurious MAY. The fix promotes an
// unresolved bitwise operand to TOP so the mask bound survives.
//
// Expectations:
//   mask_safe : a[16], i in [0,15]  -> in bounds, NO report
//   mask_oob  : a[10], i in [0,15]  -> partial overlap -> MAY (lower in-bounds)
//   => 0 MUST, 1 MAY

int mask_safe(int x)
{
    int a[16];
    int i = x & 0xF;     // i in [0,15]
    a[i] = 1;            // SAFE: [0,15] subset of valid [0,15]
    return a[0];
}

int mask_oob(int x)
{
    int a[10];           // valid [0,9]
    int i = x & 0xF;     // i in [0,15] -> partial overlap -> MAY
    a[i] = 2;
    return a[0];
}

int main(int argc, char** argv)
{
    return mask_safe(argc) + mask_oob(argc);
}
