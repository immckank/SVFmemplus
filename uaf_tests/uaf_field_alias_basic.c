#include <stdlib.h>
#include <stdio.h>

typedef struct {
    int data;
} Obj;

typedef struct {
    Obj *obj;
} Base;

/* level-3: assign tmp to base->obj, conditional free */
static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 42;
    base->obj = tmp;
    if (do_free) {
        free(tmp);
    }
}

/* level-2: pass through alias */
static void middle_wrapper(Base *base, int do_free) {
    inner_set_free(base, do_free);
}

/* level-1: external use without null check */
void trigger_uaf(int do_free) {
    Base base;
    base.obj = NULL;
    middle_wrapper(&base, do_free);
    /* UAF when do_free==1: use base.obj after tmp was freed */
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
