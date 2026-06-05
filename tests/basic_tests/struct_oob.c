// struct_oob.c -- struct field / nested array out-of-bounds.
//
// Exercises the GEP struct-field flattening path (getFlattenedElemIdx) with a
// constant field-index escape, and a nested fixed-size array inside a struct.
//
// Expected (MUST overflow): writing past the inner array bound.
// Expected (in bounds): writing the highest valid element.

struct Box {
    int  header;
    char data[8];   // valid indices [0,7]
};

int struct_oob(void)
{
    struct Box b;
    b.data[8] = 'x';   // MUST: index 8 out of [0,7]
    return b.data[0];
}

int struct_safe(void)
{
    struct Box b;
    b.data[7] = 'y';   // in bounds (highest valid)
    return b.data[7];
}

int main(void)
{
    return struct_oob() + struct_safe();
}
