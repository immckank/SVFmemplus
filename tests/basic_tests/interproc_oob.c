// interproc_oob.c -- cross-function array out-of-bounds (plan A + k=1 context).
//
// The buffer root + accumulated offset is propagated across the CallPE edge
// (actual arg -> formal param). write_at() indexes the forwarded pointer.
//
// With context-sensitive (k=1 call-site) integer-argument binding, the constant
// index forwarded through the `idx` formal is recovered precisely inside the
// callee, so an out-of-bounds store is classified as MUST (not merely MAY), and
// distinct call sites of the same callee do not pollute each other.
//
// Expectations:
//   write_at(a, 11, _) : MUST overflow  (index 11 outside [0,9])
//   write_at(a, 10, _) : MUST overflow  (index 10 outside [0,9])
//   write_at(a, 9,  _) : in bounds, no report

static void write_at(int* p, int idx, int v)
{
    p[idx] = v;   // overflow surfaces here, attributed cross-function
}

int interproc_oob(void)
{
    int a[10];
    write_at(a, 11, 42);  // MUST: index 11 is out of [0,9]
    return a[0];
}

int interproc_oob2(void)
{
    int a[10];
    write_at(a, 10, 42);  // MUST: index 10 is out of [0,9]
    return a[0];
}

int interproc_safe(void)
{
    int a[10];
    write_at(a, 9, 42);   // in bounds: must NOT be reported
    return a[9];
}

int main(void)
{
    return interproc_oob() + interproc_oob2() + interproc_safe();
}
