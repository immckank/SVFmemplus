#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 7;
    base->obj = tmp;
    if (do_free)
        free(tmp);
}

static void middle_wrapper(Base *base, int do_free) {
    inner_set_free(base, do_free);
}

void trigger_uaf(int do_free) {
    Base *base = (Base *)malloc(sizeof(Base));
    base->obj = NULL;
    middle_wrapper(base, do_free);
    printf("%d\n", base->obj->data);
    free(base);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
