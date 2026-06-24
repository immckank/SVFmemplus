#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

static void inner_set_free(Base *base, int flag) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 71;
    base->obj = tmp;
    if (flag == 1)
        free(tmp);
    else if (flag == 2)
        free(tmp);
}

static void middle_wrapper(Base *base, int flag) {
    inner_set_free(base, flag);
}

void trigger_uaf(int flag) {
    Base base;
    base.obj = NULL;
    middle_wrapper(&base, flag);
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
