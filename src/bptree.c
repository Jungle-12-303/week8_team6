#include "bptree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPTREE_MAGIC "BPTIDX1"
#define BPTREE_MAGIC_SIZE 8
#define BPTREE_HEADER_SIZE 64
#define BPTREE_NODE_SIZE (16 + (4 * BPTREE_MAX_KEYS) + (8 * (BPTREE_MAX_KEYS + 1)) + (8 * BPTREE_MAX_KEYS))

typedef struct {
    int32_t is_leaf;
    int32_t key_count;
    int64_t next_leaf_page;
    int32_t keys[BPTREE_MAX_KEYS + 1];
    int64_t children[BPTREE_MAX_KEYS + 2];
    int64_t values[BPTREE_MAX_KEYS + 1];
} DiskBPlusTreeNode;

typedef struct {
    int has_split;
    int promoted_key;
    int64_t right_page;
} InsertResult;

static long long page_offset(int64_t page_id) {
    return (long long)BPTREE_HEADER_SIZE + (long long)(page_id - 1) * (long long)BPTREE_NODE_SIZE;
}

static int set_error(char *error_buf, size_t error_buf_size, const char *message) {
    snprintf(error_buf, error_buf_size, "%s", message);
    return 0;
}

static int seek_file(FILE *file, long long offset, char *error_buf, size_t error_buf_size) {
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return set_error(error_buf, error_buf_size, "인덱스 파일 포인터를 이동하는 데 실패했습니다.");
    }
    return 1;
}

static int write_bytes(FILE *file, const void *data, size_t size, char *error_buf, size_t error_buf_size) {
    if (fwrite(data, 1, size, file) != size) {
        return set_error(error_buf, error_buf_size, "인덱스 파일에 쓰는 데 실패했습니다.");
    }
    return 1;
}

static int read_bytes(FILE *file, void *data, size_t size, char *error_buf, size_t error_buf_size) {
    if (fread(data, 1, size, file) != size) {
        return set_error(error_buf, error_buf_size, "인덱스 파일을 읽는 데 실패했습니다.");
    }
    return 1;
}

static int write_header(BPlusTree *tree, char *error_buf, size_t error_buf_size) {
    char magic[BPTREE_MAGIC_SIZE] = BPTREE_MAGIC;
    int32_t version = 1;
    int32_t max_keys = BPTREE_MAX_KEYS;
    char padding[BPTREE_HEADER_SIZE - BPTREE_MAGIC_SIZE - (2 * (int)sizeof(int32_t)) - (4 * (int)sizeof(int64_t)) - (int)sizeof(int32_t)];

    memset(padding, 0, sizeof(padding));

    if (!seek_file(tree->file, 0, error_buf, error_buf_size)) {
        return 0;
    }
    if (!write_bytes(tree->file, magic, sizeof(magic), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &version, sizeof(version), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &max_keys, sizeof(max_keys), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &tree->root_page, sizeof(tree->root_page), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &tree->first_leaf_page, sizeof(tree->first_leaf_page), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &tree->page_count, sizeof(tree->page_count), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &tree->key_count, sizeof(tree->key_count), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &tree->next_id, sizeof(tree->next_id), error_buf, error_buf_size) ||
        !write_bytes(tree->file, padding, sizeof(padding), error_buf, error_buf_size)) {
        return 0;
    }
    return 1;
}

static int read_header(BPlusTree *tree, char *error_buf, size_t error_buf_size) {
    char magic[BPTREE_MAGIC_SIZE];
    int32_t version;
    int32_t max_keys;
    int32_t next_id;
    char padding[BPTREE_HEADER_SIZE - BPTREE_MAGIC_SIZE - (2 * (int)sizeof(int32_t)) - (4 * (int)sizeof(int64_t)) - (int)sizeof(int32_t)];

    if (!seek_file(tree->file, 0, error_buf, error_buf_size)) {
        return 0;
    }
    if (!read_bytes(tree->file, magic, sizeof(magic), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &version, sizeof(version), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &max_keys, sizeof(max_keys), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &tree->root_page, sizeof(tree->root_page), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &tree->first_leaf_page, sizeof(tree->first_leaf_page), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &tree->page_count, sizeof(tree->page_count), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &tree->key_count, sizeof(tree->key_count), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &next_id, sizeof(next_id), error_buf, error_buf_size) ||
        !read_bytes(tree->file, padding, sizeof(padding), error_buf, error_buf_size)) {
        return 0;
    }

    if (memcmp(magic, BPTREE_MAGIC, BPTREE_MAGIC_SIZE) != 0) {
        return set_error(error_buf, error_buf_size, "인덱스 파일 매직이 올바르지 않습니다.");
    }
    if (version != 1) {
        return set_error(error_buf, error_buf_size, "지원하지 않는 인덱스 파일 버전입니다.");
    }
    if (max_keys != BPTREE_MAX_KEYS) {
        return set_error(error_buf, error_buf_size, "인덱스 파일 차수 설정이 현재 실행 파일과 다릅니다.");
    }

    tree->next_id = next_id;
    return 1;
}

static int write_node(BPlusTree *tree,
                      int64_t page_id,
                      const DiskBPlusTreeNode *node,
                      char *error_buf,
                      size_t error_buf_size) {
    size_t index;

    if (node->key_count < 0 || node->key_count > BPTREE_MAX_KEYS) {
        return set_error(error_buf, error_buf_size, "디스크에 기록할 수 없는 노드 크기입니다.");
    }

    if (!seek_file(tree->file, page_offset(page_id), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &node->is_leaf, sizeof(node->is_leaf), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &node->key_count, sizeof(node->key_count), error_buf, error_buf_size) ||
        !write_bytes(tree->file, &node->next_leaf_page, sizeof(node->next_leaf_page), error_buf, error_buf_size)) {
        return 0;
    }

    for (index = 0; index < BPTREE_MAX_KEYS; ++index) {
        if (!write_bytes(tree->file, &node->keys[index], sizeof(node->keys[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }
    for (index = 0; index < BPTREE_MAX_KEYS + 1; ++index) {
        if (!write_bytes(tree->file, &node->children[index], sizeof(node->children[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }
    for (index = 0; index < BPTREE_MAX_KEYS; ++index) {
        if (!write_bytes(tree->file, &node->values[index], sizeof(node->values[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }
    return 1;
}

static int read_node(BPlusTree *tree,
                     int64_t page_id,
                     DiskBPlusTreeNode *node,
                     char *error_buf,
                     size_t error_buf_size) {
    size_t index;

    memset(node, 0, sizeof(*node));

    if (page_id <= 0 || page_id > tree->page_count) {
        return set_error(error_buf, error_buf_size, "유효하지 않은 인덱스 페이지 번호입니다.");
    }

    if (!seek_file(tree->file, page_offset(page_id), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &node->is_leaf, sizeof(node->is_leaf), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &node->key_count, sizeof(node->key_count), error_buf, error_buf_size) ||
        !read_bytes(tree->file, &node->next_leaf_page, sizeof(node->next_leaf_page), error_buf, error_buf_size)) {
        return 0;
    }

    for (index = 0; index < BPTREE_MAX_KEYS; ++index) {
        if (!read_bytes(tree->file, &node->keys[index], sizeof(node->keys[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }
    for (index = 0; index < BPTREE_MAX_KEYS + 1; ++index) {
        if (!read_bytes(tree->file, &node->children[index], sizeof(node->children[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }
    for (index = 0; index < BPTREE_MAX_KEYS; ++index) {
        if (!read_bytes(tree->file, &node->values[index], sizeof(node->values[index]), error_buf, error_buf_size)) {
            return 0;
        }
    }

    if (node->key_count < 0 || node->key_count > BPTREE_MAX_KEYS) {
        return set_error(error_buf, error_buf_size, "인덱스 노드의 key 수가 손상되었습니다.");
    }
    return 1;
}

static int64_t allocate_page(BPlusTree *tree, char *error_buf, size_t error_buf_size) {
    int64_t page_id = tree->page_count + 1;
    DiskBPlusTreeNode zero_node;

    memset(&zero_node, 0, sizeof(zero_node));
    tree->page_count = page_id;
    if (!write_header(tree, error_buf, error_buf_size) ||
        !write_node(tree, page_id, &zero_node, error_buf, error_buf_size)) {
        return 0;
    }
    return page_id;
}

static size_t find_insert_index(const DiskBPlusTreeNode *node, int key) {
    size_t index = 0;

    while (index < (size_t)node->key_count && node->keys[index] < key) {
        ++index;
    }

    return index;
}

static size_t find_child_index(const DiskBPlusTreeNode *node, int key) {
    size_t index = 0;

    while (index < (size_t)node->key_count && key >= node->keys[index]) {
        ++index;
    }

    return index;
}

static int64_t find_leaf_page(BPlusTree *tree, int key, char *error_buf, size_t error_buf_size) {
    int64_t page_id = tree->root_page;
    DiskBPlusTreeNode node;

    while (page_id != 0) {
        if (!read_node(tree, page_id, &node, error_buf, error_buf_size)) {
            return -1;
        }
        if (node.is_leaf) {
            return page_id;
        }
        page_id = node.children[find_child_index(&node, key)];
    }

    return 0;
}

static InsertResult make_no_split(void) {
    InsertResult result;

    result.has_split = 0;
    result.promoted_key = 0;
    result.right_page = 0;
    return result;
}

static InsertResult insert_recursive(BPlusTree *tree,
                                     int64_t page_id,
                                     int key,
                                     long value,
                                     int *duplicate_key,
                                     char *error_buf,
                                     size_t error_buf_size) {
    DiskBPlusTreeNode node;
    InsertResult result = make_no_split();
    size_t index;

    if (!read_node(tree, page_id, &node, error_buf, error_buf_size)) {
        *duplicate_key = -1;
        return result;
    }

    if (node.is_leaf) {
        int64_t right_page;
        DiskBPlusTreeNode right_node;
        size_t split_index;
        size_t right_count;

        index = find_insert_index(&node, key);
        if (index < (size_t)node.key_count && node.keys[index] == key) {
            *duplicate_key = 1;
            return result;
        }

        for (; index < (size_t)node.key_count; ++index) {
            if (node.keys[index] >= key) {
                break;
            }
        }
        for (size_t shift = (size_t)node.key_count; shift > index; --shift) {
            node.keys[shift] = node.keys[shift - 1];
            node.values[shift] = node.values[shift - 1];
        }

        node.keys[index] = key;
        node.values[index] = value;
        ++node.key_count;

        if (node.key_count <= BPTREE_MAX_KEYS) {
            if (!write_node(tree, page_id, &node, error_buf, error_buf_size)) {
                *duplicate_key = -1;
            }
            return result;
        }

        memset(&right_node, 0, sizeof(right_node));
        right_node.is_leaf = 1;

        split_index = (size_t)node.key_count / 2;
        right_count = (size_t)node.key_count - split_index;
        for (index = 0; index < right_count; ++index) {
            right_node.keys[index] = node.keys[split_index + index];
            right_node.values[index] = node.values[split_index + index];
        }
        right_node.key_count = (int32_t)right_count;
        node.key_count = (int32_t)split_index;

        right_page = allocate_page(tree, error_buf, error_buf_size);
        if (right_page == 0) {
            *duplicate_key = -1;
            return result;
        }

        right_node.next_leaf_page = node.next_leaf_page;
        node.next_leaf_page = right_page;

        if (!write_node(tree, page_id, &node, error_buf, error_buf_size) ||
            !write_node(tree, right_page, &right_node, error_buf, error_buf_size)) {
            *duplicate_key = -1;
            return result;
        }

        result.has_split = 1;
        result.promoted_key = right_node.keys[0];
        result.right_page = right_page;
        return result;
    }

    index = find_child_index(&node, key);
    {
        InsertResult child_result = insert_recursive(tree,
                                                     node.children[index],
                                                     key,
                                                     value,
                                                     duplicate_key,
                                                     error_buf,
                                                     error_buf_size);
        if (*duplicate_key != 0 || !child_result.has_split) {
            return child_result.has_split ? child_result : result;
        }

        for (size_t shift = (size_t)node.key_count; shift > index; --shift) {
            node.keys[shift] = node.keys[shift - 1];
        }
        for (size_t shift = (size_t)node.key_count + 1; shift > index + 1; --shift) {
            node.children[shift] = node.children[shift - 1];
        }

        node.keys[index] = child_result.promoted_key;
        node.children[index + 1] = child_result.right_page;
        ++node.key_count;
    }

    if (node.key_count <= BPTREE_MAX_KEYS) {
        if (!write_node(tree, page_id, &node, error_buf, error_buf_size)) {
            *duplicate_key = -1;
        }
        return result;
    }

    {
        DiskBPlusTreeNode right_node;
        int64_t right_page;
        size_t split_index;
        size_t right_key_count;

        memset(&right_node, 0, sizeof(right_node));
        right_node.is_leaf = 0;

        split_index = (size_t)node.key_count / 2;
        result.promoted_key = node.keys[split_index];
        right_key_count = (size_t)node.key_count - split_index - 1;
        for (index = 0; index < right_key_count; ++index) {
            right_node.keys[index] = node.keys[split_index + 1 + index];
        }
        for (index = 0; index <= right_key_count; ++index) {
            right_node.children[index] = node.children[split_index + 1 + index];
        }

        right_node.key_count = (int32_t)right_key_count;
        node.key_count = (int32_t)split_index;

        right_page = allocate_page(tree, error_buf, error_buf_size);
        if (right_page == 0) {
            *duplicate_key = -1;
            result.has_split = 0;
            return result;
        }

        if (!write_node(tree, page_id, &node, error_buf, error_buf_size) ||
            !write_node(tree, right_page, &right_node, error_buf, error_buf_size)) {
            *duplicate_key = -1;
            result.has_split = 0;
            return result;
        }

        result.has_split = 1;
        result.right_page = right_page;
        return result;
    }
}

static size_t lower_bound_in_leaf(const DiskBPlusTreeNode *leaf, int key) {
    size_t index = 0;

    while (index < (size_t)leaf->key_count && leaf->keys[index] < key) {
        ++index;
    }

    return index;
}

static size_t upper_bound_in_leaf(const DiskBPlusTreeNode *leaf, int key) {
    size_t index = 0;

    while (index < (size_t)leaf->key_count && leaf->keys[index] <= key) {
        ++index;
    }

    return index;
}

static int visit_from_leaf(BPlusTree *tree,
                           int64_t page_id,
                           size_t start_index,
                           BPlusTreeVisitFn visit,
                           void *context,
                           char *error_buf,
                           size_t error_buf_size) {
    DiskBPlusTreeNode leaf;
    size_t index;

    while (page_id != 0) {
        if (!read_node(tree, page_id, &leaf, error_buf, error_buf_size)) {
            return 0;
        }

        for (index = start_index; index < (size_t)leaf.key_count; ++index) {
            if (!visit(leaf.keys[index], (long)leaf.values[index], context)) {
                return 0;
            }
        }

        page_id = leaf.next_leaf_page;
        start_index = 0;
    }

    return 1;
}

void bptree_init(BPlusTree *tree) {
    if (tree == NULL) {
        return;
    }

    memset(tree, 0, sizeof(*tree));
    tree->next_id = 1;
}

void bptree_destroy(BPlusTree *tree) {
    if (tree == NULL) {
        return;
    }

    if (tree->file != NULL) {
        fclose(tree->file);
    }

    bptree_init(tree);
}

int bptree_open(BPlusTree *tree, const char *path, char *error_buf, size_t error_buf_size) {
    FILE *file = fopen(path, "r+b");
    int new_file = 0;
    long file_size = 0;

    if (error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    bptree_destroy(tree);

    if (file == NULL) {
        file = fopen(path, "w+b");
        if (file == NULL) {
            snprintf(error_buf, error_buf_size, "인덱스 파일을 열 수 없습니다: %s", path);
            return 0;
        }
        new_file = 1;
    }

    tree->file = file;
    setvbuf(tree->file, NULL, _IOFBF, 1 << 20);
    if (fseek(tree->file, 0, SEEK_END) != 0) {
        bptree_destroy(tree);
        return set_error(error_buf, error_buf_size, "인덱스 파일 크기를 확인하는 데 실패했습니다.");
    }
    file_size = ftell(tree->file);
    if (file_size < 0) {
        bptree_destroy(tree);
        return set_error(error_buf, error_buf_size, "인덱스 파일 크기를 읽는 데 실패했습니다.");
    }
    if (file_size == 0) {
        new_file = 1;
    }

    if (new_file) {
        tree->root_page = 0;
        tree->first_leaf_page = 0;
        tree->page_count = 0;
        tree->key_count = 0;
        tree->next_id = 1;
        if (!write_header(tree, error_buf, error_buf_size)) {
            bptree_destroy(tree);
            return 0;
        }
        return 1;
    }

    if (!read_header(tree, error_buf, error_buf_size)) {
        bptree_destroy(tree);
        return 0;
    }

    return 1;
}

size_t bptree_size(const BPlusTree *tree) {
    return tree == NULL ? 0 : (size_t)tree->key_count;
}

int bptree_next_id(const BPlusTree *tree) {
    return tree == NULL ? 1 : tree->next_id;
}

int bptree_insert(BPlusTree *tree, int key, long value, char *error_buf, size_t error_buf_size) {
    int duplicate_key = 0;
    InsertResult result;

    if (error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    if (tree == NULL || tree->file == NULL) {
        return set_error(error_buf, error_buf_size, "인덱스 파일이 열려 있지 않습니다.");
    }

    if (tree->root_page == 0) {
        DiskBPlusTreeNode root_node;
        int64_t root_page = allocate_page(tree, error_buf, error_buf_size);

        if (root_page == 0) {
            return 0;
        }

        memset(&root_node, 0, sizeof(root_node));
        root_node.is_leaf = 1;
        root_node.key_count = 1;
        root_node.keys[0] = key;
        root_node.values[0] = value;

        tree->root_page = root_page;
        tree->first_leaf_page = root_page;
        tree->key_count = 1;
        if (key >= tree->next_id) {
            tree->next_id = key + 1;
        }

        if (!write_node(tree, root_page, &root_node, error_buf, error_buf_size) ||
            !write_header(tree, error_buf, error_buf_size)) {
            return 0;
        }

        return 1;
    }

    result = insert_recursive(tree, tree->root_page, key, value, &duplicate_key, error_buf, error_buf_size);
    if (duplicate_key == 1) {
        snprintf(error_buf, error_buf_size, "중복된 id 키입니다: %d", key);
        return 0;
    }
    if (duplicate_key == -1) {
        return 0;
    }

    if (result.has_split) {
        DiskBPlusTreeNode new_root;
        int64_t new_root_page = allocate_page(tree, error_buf, error_buf_size);

        if (new_root_page == 0) {
            return 0;
        }

        memset(&new_root, 0, sizeof(new_root));
        new_root.is_leaf = 0;
        new_root.key_count = 1;
        new_root.keys[0] = result.promoted_key;
        new_root.children[0] = tree->root_page;
        new_root.children[1] = result.right_page;

        if (!write_node(tree, new_root_page, &new_root, error_buf, error_buf_size)) {
            return 0;
        }
        tree->root_page = new_root_page;
    }

    ++tree->key_count;
    if (key >= tree->next_id) {
        tree->next_id = key + 1;
    }

    return write_header(tree, error_buf, error_buf_size);
}

int bptree_search(BPlusTree *tree, int key, long *out_value, char *error_buf, size_t error_buf_size) {
    int64_t leaf_page;
    DiskBPlusTreeNode leaf;
    size_t index;

    if (error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    if (tree == NULL || tree->file == NULL || tree->root_page == 0) {
        return 0;
    }

    leaf_page = find_leaf_page(tree, key, error_buf, error_buf_size);
    if (leaf_page <= 0) {
        return 0;
    }
    if (!read_node(tree, leaf_page, &leaf, error_buf, error_buf_size)) {
        return 0;
    }

    for (index = 0; index < (size_t)leaf.key_count; ++index) {
        if (leaf.keys[index] == key) {
            if (out_value != NULL) {
                *out_value = (long)leaf.values[index];
            }
            return 1;
        }
    }

    return 0;
}

int bptree_visit(BPlusTree *tree,
                 CompareOperator op,
                 int key,
                 BPlusTreeVisitFn visit,
                 void *context,
                 char *error_buf,
                 size_t error_buf_size) {
    DiskBPlusTreeNode leaf;
    DiskBPlusTreeNode current_leaf;
    int64_t leaf_page;
    int64_t current_page;
    long value;
    size_t index;

    if (error_buf_size > 0) {
        error_buf[0] = '\0';
    }
    if (tree == NULL || tree->file == NULL || visit == NULL || tree->root_page == 0) {
        return 1;
    }

    switch (op) {
        case COMPARE_EQUALS:
            if (!bptree_search(tree, key, &value, error_buf, error_buf_size)) {
                return error_buf[0] == '\0' ? 1 : 0;
            }
            return visit(key, value, context);
        case COMPARE_GREATER_THAN:
        case COMPARE_GREATER_THAN_OR_EQUAL:
            leaf_page = find_leaf_page(tree, key, error_buf, error_buf_size);
            if (leaf_page <= 0) {
                return leaf_page == 0 ? 1 : 0;
            }
            if (!read_node(tree, leaf_page, &leaf, error_buf, error_buf_size)) {
                return 0;
            }
            return visit_from_leaf(tree,
                                   leaf_page,
                                   op == COMPARE_GREATER_THAN ? upper_bound_in_leaf(&leaf, key) : lower_bound_in_leaf(&leaf, key),
                                   visit,
                                   context,
                                   error_buf,
                                   error_buf_size);
        case COMPARE_LESS_THAN:
        case COMPARE_LESS_THAN_OR_EQUAL:
            current_page = tree->first_leaf_page;
            while (current_page != 0) {
                if (!read_node(tree, current_page, &current_leaf, error_buf, error_buf_size)) {
                    return 0;
                }
                for (index = 0; index < (size_t)current_leaf.key_count; ++index) {
                    if ((op == COMPARE_LESS_THAN && current_leaf.keys[index] >= key) ||
                        (op == COMPARE_LESS_THAN_OR_EQUAL && current_leaf.keys[index] > key)) {
                        return 1;
                    }
                    if (!visit(current_leaf.keys[index], (long)current_leaf.values[index], context)) {
                        return 0;
                    }
                }
                current_page = current_leaf.next_leaf_page;
            }
            return 1;
        case COMPARE_NOT_EQUALS:
            current_page = tree->first_leaf_page;
            while (current_page != 0) {
                if (!read_node(tree, current_page, &current_leaf, error_buf, error_buf_size)) {
                    return 0;
                }
                for (index = 0; index < (size_t)current_leaf.key_count; ++index) {
                    if (current_leaf.keys[index] == key) {
                        continue;
                    }
                    if (!visit(current_leaf.keys[index], (long)current_leaf.values[index], context)) {
                        return 0;
                    }
                }
                current_page = current_leaf.next_leaf_page;
            }
            return 1;
    }

    return 1;
}
