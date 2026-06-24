#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *inner_obj; } Inner;
typedef struct { Inner holder; } Outer;

static void inner_set_free(Outer *outer, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 67;
    outer->holder.inner_obj = tmp;
    if (do_free)
        free(tmp);
}

static void middle_wrapper(Outer *outer, int do_free) {
    inner_set_free(outer, do_free);
}

void trigger_uaf(int do_free) {
    Outer outer;
    outer.holder.inner_obj = NULL;
    middle_wrapper(&outer, do_free);
    printf("%d\n", outer.holder.inner_obj->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
