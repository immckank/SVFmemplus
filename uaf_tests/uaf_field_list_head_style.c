#include <stdlib.h>
#include <stdio.h>

typedef struct Node {
    struct Node *next;
    int data;
} Node;

typedef struct {
    Node *head;
} Container;

static void attach_and_maybe_free(Container *c, int do_free) {
    Node *tmp = (Node *)malloc(sizeof(Node));
    tmp->data = 83;
    tmp->next = NULL;
    c->head = tmp;
    if (do_free)
        free(tmp);
}

static void setup(Container *c, int do_free) {
    attach_and_maybe_free(c, do_free);
}

void trigger_uaf(int do_free) {
    Container c;
    c.head = NULL;
    setup(&c, do_free);
    printf("%d\n", c.head->data);
}

int main(void) {
    trigger_uaf(1);
    return 0;
}
