#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *slots[1]; } Container;

static void inner_set_free(Container *c, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 59;
    c->slots[0] = tmp;
    if (do_free)
        free(tmp);
}

static void middle_wrapper(Container *c, int do_free) {
    inner_set_free(c, do_free);
}

void trigger_uaf(int do_free) {
    Container c;
    c.slots[0] = NULL;
    middle_wrapper(&c, do_free);
    printf("%d\n", c.slots[0]->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
