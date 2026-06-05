// single_byte.c -- single-byte buffer regression (size>=1 boundary fix).
//
// Before the fix, buffers of size 1 (size>1 guard) were ignored entirely.
//
// Expected (MUST overflow): write to b[1] on char b[1].
// Expected (in bounds):     write to b[0].

int single_byte_oob(void)
{
    char b[1];
    b[1] = 'x';   // MUST: index 1 out of [0,0]
    return b[0];
}

int single_byte_safe(void)
{
    char b[1];
    b[0] = 'y';   // in bounds
    return b[0];
}

int main(void)
{
    return single_byte_oob() + single_byte_safe();
}
