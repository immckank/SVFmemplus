// stack_oob.c -- stack array out-of-bounds regression cases.
//
// Expected (MUST overflow):
//   - write_oob:  a[10] on int a[10]  -> index 10 out of [0,9]
//   - read_oob:   return a[16]        -> index 16 out of [0,9]
// Expected (in bounds, no report):
//   - safe:       a[9]                -> highest valid index

int safe(void)
{
    int a[10];
    for (int i = 0; i < 10; i++)
        a[i] = i;
    return a[9];
}

int write_oob(void)
{
    int a[10];
    a[10] = 42;   // MUST: index 10 out of [0,9]
    return a[0];
}

int read_oob(void)
{
    int a[10];
    return a[16]; // MUST: index 16 out of [0,9]
}

int main(void)
{
    return safe() + write_oob() + read_oob();
}
