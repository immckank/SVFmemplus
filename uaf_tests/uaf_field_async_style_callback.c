#include <stdlib.h>
#include <stdio.h>

typedef struct { int data; } Obj;
typedef struct { Obj *obj; } Base;

typedef void (*cb_t)(Base *);

static cb_t g_cb;

static void inner_set_free(Base *base, int do_free) {
    Obj *tmp = (Obj *)malloc(sizeof(Obj));
    tmp->data = 47;
    base->obj = tmp;
    if (do_free)
        free(tmp);
}

static void middle_register(Base *base, int do_free, cb_t cb) {
    inner_set_free(base, do_free);
    g_cb = cb;
}

static void use_callback(Base *base) {
    (void)base;
    printf("%d\n", g_cb ? 0 : 0);
}

static void real_use(Base *base) {
    printf("%d\n", base->obj->data);
}

void trigger_uaf(int do_free) {
    Base base;
    base.obj = NULL;
    middle_register(&base, do_free, real_use);
    use_callback(&base);
    g_cb(&base);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
