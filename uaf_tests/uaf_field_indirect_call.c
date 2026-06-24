#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

typedef void (*inner_fn_t)(Base *, int);

static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 29;
    base->obj = tmp;
    if (do_free)
        free(tmp);
}

static void middle_dispatch(Base *base, int do_free, inner_fn_t fn) {
    fn(base, do_free);
}

void trigger_uaf(int do_free) {
    Base base;
    base.obj = NULL;
    middle_dispatch(&base, do_free, inner_set_free);
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
