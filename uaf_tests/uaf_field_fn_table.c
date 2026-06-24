#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;
typedef void (*handler_t)(Base *, int);

static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 97;
    base->obj = tmp;
    if (do_free)
        free(tmp);
}

static const handler_t g_table[] = { inner_set_free };

static void middle_via_table(Base *base, int do_free) {
    g_table[0](base, do_free);
}

void trigger_uaf(int do_free) {
    Base base;
    base.obj = NULL;
    middle_via_table(&base, do_free);
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
