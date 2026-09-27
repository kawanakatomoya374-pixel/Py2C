#include "runtime/python_code_to_c_runtime.h"

static void create_unrooted_cycle(void) {
    P2C_Object *left = p2c_list_new();
    P2C_Object *right = p2c_list_new();
    if (!left || !right) return;
    p2c_list_append(left, right);
    p2c_list_append(right, left);
}

static int exercise_epoch(void) {
    P2C_Object *root = p2c_list_new();
    if (!root) return 20;
    p2c_gc_register_root(&root);
    p2c_gc_register_root(&root);
    if (p2c_gc_root_count() != 1) return 21;

    for (int i = 0; i < 96; i++) {
        P2C_Object *child = p2c_list_new();
        if (!child) return 22;
        p2c_list_append(child, p2c_obj_from_int(i));
        p2c_list_append(root, child);
        if ((i % 8) == 0) p2c_gc_collect();
    }
    p2c_obj_incref(root);
    p2c_gc_collect();
    p2c_obj_decref(root);
    p2c_gc_unregister_root(&root);
    if (p2c_gc_root_count() != 0) return 23;
    root = NULL;
    create_unrooted_cycle();
    p2c_gc_collect();
    return p2c_gc_is_collecting() ? 24 : 0;
}

int main(void) {
    int stack_mark = 0;
    for (int epoch = 0; epoch < 3; epoch++) {
        p2c_runtime_init(NULL, 0);
        if (!p2c_runtime_is_active()) return 10;
        p2c_gc_init(&stack_mark);
        p2c_gc_set_threshold(1);
        if (!p2c_class_new("ReinitProbe", NULL, NULL, NULL)) return 11;
        int status = exercise_epoch();
        if (status != 0) return status;
        p2c_runtime_shutdown();
        if (p2c_runtime_is_active()) return 12;
        if (p2c_gc_root_count() != 0 || p2c_gc_object_count() != 0) return 13;
    }
    return 0;
}
