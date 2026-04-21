#ifndef BPTREE_H
#define BPTREE_H

#include "ast.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define BPTREE_ORDER 64
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

typedef struct {
    FILE *file;
    int64_t root_page;
    int64_t first_leaf_page;
    int64_t page_count;
    int64_t key_count;
    int next_id;
} BPlusTree;

typedef int (*BPlusTreeVisitFn)(int key, long value, void *context);

void bptree_init(BPlusTree *tree);
void bptree_destroy(BPlusTree *tree);
int bptree_open(BPlusTree *tree, const char *path, char *error_buf, size_t error_buf_size);
size_t bptree_size(const BPlusTree *tree);
int bptree_next_id(const BPlusTree *tree);
int bptree_insert(BPlusTree *tree, int key, long value, char *error_buf, size_t error_buf_size);
int bptree_search(BPlusTree *tree, int key, long *out_value, char *error_buf, size_t error_buf_size);
int bptree_visit(BPlusTree *tree,
                 CompareOperator op,
                 int key,
                 BPlusTreeVisitFn visit,
                 void *context,
                 char *error_buf,
                 size_t error_buf_size);

#endif
