#include <stddef.h>
#include <stdlib.h>

#define OK 0

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct vfs_node {
    int id;
} vfs_node_t;

struct object {
    int value;
};

struct base {
    struct object *obj;
    struct object *objobj;
};

struct extent_tree {
    int data;
};

struct vnode {
    int flags;
    vfs_node_t vnode;
    struct extent_tree *extent_tree;
    int other;
};

struct node_ops {
    void (*node_destroy)(vfs_node_t *vn);
    void (*make_bad)(vfs_node_t *vn);
};

extern int external_status(void);
extern void external_use(void *);

static void *malloc_api(size_t size)
{
    return malloc(size);
}

static int func_callee(void *ptr1, struct base *base)
{
    external_use(ptr1);
    external_use(base);
    return external_status();
}

static inline struct vnode *VNODE(const vfs_node_t *vnode)
{
    return container_of(vnode, struct vnode, vnode);
}

static void baseline_direct_dfree(void)
{
    void *p = malloc_api(16);
    if (p == NULL) {
        return;
    }
    free(p);
    free(p);
}

static void member_error_path_dfree(void *ptr1)
{
    struct base *base = (struct base *)malloc_api(sizeof(*base));
    int ret_msg;

    if (base == NULL) {
        return;
    }

    base->obj = (struct object *)malloc_api(sizeof(*base->obj));
    if (base->obj == NULL) {
        free(base);
        return;
    }

    base->objobj = (struct object *)ptr1;
    ret_msg = func_callee(ptr1, base);
    if (ret_msg != OK) {
        free(base->obj);
        free(base->obj);
        free(base);
        return;
    }

    free(base->obj);
    free(base);
}

void destroy_extent_tree(vfs_node_t *vn)
{
    struct extent_tree *et = VNODE(vn)->extent_tree;

    external_use(vn);
    free(et);
    free(et);
    VNODE(vn)->extent_tree = NULL;
}

void destroy_extent_tree_by_vnode(struct vnode *node)
{
    struct extent_tree *et = node->extent_tree;

    external_use(node);
    free(et);
    free(et);
    node->extent_tree = NULL;
}

void vnode_destroy(vfs_node_t *vn)
{
    destroy_extent_tree(vn);
}

void make_bad_node(vfs_node_t *vn)
{
    destroy_extent_tree(vn);
}

const struct node_ops node_ops = {
    .node_destroy = vnode_destroy,
    .make_bad = make_bad_node,
};

static void direct_vnode_field_dfree(void)
{
    struct vnode *node = (struct vnode *)malloc_api(sizeof(*node));
    struct extent_tree *tree = (struct extent_tree *)malloc_api(sizeof(*tree));

    if (node == NULL || tree == NULL) {
        free(tree);
        free(node);
        return;
    }

    node->extent_tree = tree;
    destroy_extent_tree_by_vnode(node);
    free(node);
}

static void direct_container_of_dfree(void)
{
    struct vnode *node = (struct vnode *)malloc_api(sizeof(*node));
    struct extent_tree *tree = (struct extent_tree *)malloc_api(sizeof(*tree));

    if (node == NULL || tree == NULL) {
        free(tree);
        free(node);
        return;
    }

    node->extent_tree = tree;
    destroy_extent_tree(&node->vnode);
    free(node);
}

static void function_pointer_container_of_dfree(int choose_make_bad)
{
    struct vnode *node = (struct vnode *)malloc_api(sizeof(*node));
    struct extent_tree *tree = (struct extent_tree *)malloc_api(sizeof(*tree));

    if (node == NULL || tree == NULL) {
        free(tree);
        free(node);
        return;
    }

    node->extent_tree = tree;
    if (choose_make_bad) {
        node_ops.make_bad(&node->vnode);
    } else {
        node_ops.node_destroy(&node->vnode);
    }

    free(node);
}

int main(int argc, char **argv)
{
    baseline_direct_dfree();
    member_error_path_dfree(argv);
    direct_vnode_field_dfree();
    direct_container_of_dfree();
    function_pointer_container_of_dfree(argc);
    return 0;
}
