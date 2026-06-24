#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 43;
    base->obj = tmp;
    if (do_free) {
        free(tmp);
        base->obj = NULL;
    }
}

static void middle_wrapper(Base *base, int do_free) {
    inner_set_free(base, do_free);
}

void trigger_uaf_missed(int do_free) {
    Base base;
    base.obj = NULL;
    middle_wrapper(&base, do_free);
    /* do_free==1 path sets obj=NULL, so this is NOT UAF - should not report */
    if (base.obj)
        printf("%d\n", base.obj->data);
}

void trigger_uaf_real(int do_free) {
    Base base;
    base.obj = NULL;
    middle_wrapper(&base, do_free);
    /* missing null check after partial cleanup */
    printf("%d\n", base.obj->data);
}

int main(void) {
    trigger_uaf_missed(1);
    trigger_uaf_real(1);
    return 0;
}
