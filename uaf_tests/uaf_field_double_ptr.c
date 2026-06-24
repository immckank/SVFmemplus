#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

static void inner_set_free(Base **basepp, int do_free) {
    Base *base = *basepp;
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 13;
    base->obj = tmp;
    if (do_free)
        free(tmp);
}

static void middle_wrapper(Base **basepp, int do_free) {
    inner_set_free(basepp, do_free);
}

void trigger_uaf(int do_free) {
    Base base;
    base.obj = NULL;
    Base *bp = &base;
    middle_wrapper(&bp, do_free);
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
