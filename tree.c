#include "subsync.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *name;
    NodeKind kind;
} DirEntry;

typedef struct {
    const char *source_root;
    const char *target_root;
    size_t skipped_source_entries;
    size_t skipped_target_entries;
    char *err;
    size_t err_size;
} BuildContext;

static void set_error(BuildContext *ctx, const char *fmt, ...) {
    va_list args;

    if (ctx->err == NULL || ctx->err_size == 0) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(ctx->err, ctx->err_size, fmt, args);
    va_end(args);
}

static char *dup_string(const char *value) {
    size_t len;
    char *copy;

    if (value == NULL) {
        return NULL;
    }

    len = strlen(value);
    copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, len + 1);
    return copy;
}

static char *path_join(const char *base, const char *relative_path) {
    size_t base_len;
    size_t rel_len;
    char *joined;

    if (relative_path == NULL || relative_path[0] == '\0') {
        return dup_string(base);
    }

    base_len = strlen(base);
    rel_len = strlen(relative_path);
    joined = malloc(base_len + rel_len + 2);
    if (joined == NULL) {
        return NULL;
    }

    memcpy(joined, base, base_len);
    joined[base_len] = '/';
    memcpy(joined + base_len + 1, relative_path, rel_len + 1);
    return joined;
}

static char *relative_join(const char *parent, const char *name) {
    size_t parent_len;
    size_t name_len;
    char *joined;

    if (parent == NULL || parent[0] == '\0') {
        return dup_string(name);
    }

    parent_len = strlen(parent);
    name_len = strlen(name);
    joined = malloc(parent_len + name_len + 2);
    if (joined == NULL) {
        return NULL;
    }

    memcpy(joined, parent, parent_len);
    joined[parent_len] = '/';
    memcpy(joined + parent_len + 1, name, name_len + 1);
    return joined;
}

static char *basename_copy(const char *path) {
    const char *last_slash;

    if (strcmp(path, "/") == 0) {
        return dup_string("/");
    }

    last_slash = strrchr(path, '/');
    return dup_string(last_slash == NULL ? path : last_slash + 1);
}

static void free_dir_entries(DirEntry *entries, size_t count) {
    size_t i;

    if (entries == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(entries[i].name);
    }
    free(entries);
}

static int classify_source_entry(const char *path, NodeKind *kind, bool *supported) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        *kind = NODE_DIR;
        *supported = true;
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        *kind = NODE_FILE;
        *supported = true;
        return 0;
    }

    *supported = false;
    return 0;
}

static int classify_target_path(const char *path, NodeKind *kind, bool *exists) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            *exists = false;
            return 0;
        }
        return -1;
    }

    *exists = true;
    *kind = S_ISDIR(st.st_mode) ? NODE_DIR : NODE_FILE;
    return 0;
}

static int read_directory_entries(
    BuildContext *ctx,
    const char *path,
    bool source_side,
    DirEntry **out_entries,
    size_t *out_count
) {
    DIR *dir;
    struct dirent *entry;
    DirEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int saved_errno = 0;

    *out_entries = NULL;
    *out_count = 0;

    dir = opendir(path);
    if (dir == NULL) {
        set_error(ctx, "Cannot open directory '%s': %s", path, strerror(errno));
        return -1;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *entry_path;
        NodeKind kind = NODE_FILE;
        bool supported = true;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        entry_path = path_join(path, entry->d_name);
        if (entry_path == NULL) {
            closedir(dir);
            set_error(ctx, "Out of memory while reading '%s'", path);
            free_dir_entries(entries, count);
            return -1;
        }

        if (source_side) {
            if (classify_source_entry(entry_path, &kind, &supported) != 0) {
                saved_errno = errno;
                free(entry_path);
                closedir(dir);
                set_error(ctx, "Cannot inspect '%s': %s", path, strerror(saved_errno));
                free_dir_entries(entries, count);
                return -1;
            }
            if (!supported) {
                ++ctx->skipped_source_entries;
                free(entry_path);
                continue;
            }
        } else {
            bool exists = false;

            if (classify_target_path(entry_path, &kind, &exists) != 0) {
                saved_errno = errno;
                free(entry_path);
                closedir(dir);
                set_error(ctx, "Cannot inspect '%s': %s", path, strerror(saved_errno));
                free_dir_entries(entries, count);
                return -1;
            }
            if (!exists) {
                free(entry_path);
                continue;
            }
        }

        free(entry_path);

        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 16 : capacity * 2;
            DirEntry *grown = realloc(entries, next_capacity * sizeof(*entries));
            if (grown == NULL) {
                closedir(dir);
                set_error(ctx, "Out of memory while reading '%s'", path);
                free_dir_entries(entries, count);
                return -1;
            }
            entries = grown;
            capacity = next_capacity;
        }

        entries[count].name = dup_string(entry->d_name);
        if (entries[count].name == NULL) {
            closedir(dir);
            set_error(ctx, "Out of memory while reading '%s'", path);
            free_dir_entries(entries, count);
            return -1;
        }
        entries[count].kind = kind;
        ++count;
    }

    saved_errno = errno;
    closedir(dir);
    if (saved_errno != 0) {
        set_error(ctx, "Cannot read directory '%s': %s", path, strerror(saved_errno));
        free_dir_entries(entries, count);
        return -1;
    }

    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int node_compare(const void *lhs, const void *rhs) {
    const Node *left = *(const Node *const *)lhs;
    const Node *right = *(const Node *const *)rhs;

    if (left->kind != right->kind) {
        return left->kind == NODE_DIR ? -1 : 1;
    }
    if (left->origin != right->origin) {
        return left->origin == ORIGIN_SOURCE ? -1 : 1;
    }
    return strcmp(left->name, right->name);
}

static Node *node_create(
    const char *name,
    const char *relative_path,
    NodeKind kind,
    NodeOrigin origin,
    Node *parent
) {
    Node *node = calloc(1, sizeof(*node));

    if (node == NULL) {
        return NULL;
    }

    node->name = dup_string(name);
    node->relative_path = dup_string(relative_path);
    if (node->name == NULL || node->relative_path == NULL) {
        free(node->name);
        free(node->relative_path);
        free(node);
        return NULL;
    }

    node->kind = kind;
    node->origin = origin;
    node->parent = parent;
    return node;
}

static void node_destroy(Node *node) {
    size_t i;

    if (node == NULL) {
        return;
    }

    for (i = 0; i < node->child_count; ++i) {
        node_destroy(node->children[i]);
    }

    free(node->children);
    free(node->name);
    free(node->relative_path);
    free(node);
}

void tree_free(Tree *tree) {
    if (tree == NULL) {
        return;
    }

    node_destroy(tree->root);
    free(tree);
}

static int node_add_child(Node *parent, Node *child) {
    Node **grown = realloc(parent->children, (parent->child_count + 1) * sizeof(*parent->children));

    if (grown == NULL) {
        return -1;
    }

    parent->children = grown;
    parent->children[parent->child_count] = child;
    ++parent->child_count;
    return 0;
}

static bool name_exists(const DirEntry *entries, size_t count, const char *name) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static size_t count_target_items_recursive(BuildContext *ctx, const char *path) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return 0;
        }
        set_error(ctx, "Cannot inspect '%s': %s", path, strerror(errno));
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        return 1;
    }

    DirEntry *entries = NULL;
    size_t count = 0;
    size_t total = 1;
    size_t i;

    if (read_directory_entries(ctx, path, false, &entries, &count) != 0) {
        return 0;
    }

    for (i = 0; i < count; ++i) {
        char *child_path = path_join(path, entries[i].name);
        size_t child_total;

        if (child_path == NULL) {
            set_error(ctx, "Out of memory while reading '%s'", path);
            free_dir_entries(entries, count);
            return 0;
        }

        child_total = count_target_items_recursive(ctx, child_path);
        free(child_path);
        if (ctx->err[0] != '\0') {
            free_dir_entries(entries, count);
            return 0;
        }

        total += child_total;
    }

    free_dir_entries(entries, count);
    return total;
}

static Node *build_target_only_node(
    BuildContext *ctx,
    const char *relative_path,
    const char *name,
    NodeKind kind,
    Node *parent
) {
    Node *node = node_create(name, relative_path, kind, ORIGIN_TARGET_ONLY, parent);

    if (node == NULL) {
        set_error(ctx, "Out of memory while building the tree");
        return NULL;
    }

    if (kind == NODE_DIR) {
        char *target_path = path_join(ctx->target_root, relative_path);
        DirEntry *entries = NULL;
        size_t count = 0;
        size_t i;

        if (target_path == NULL) {
            set_error(ctx, "Out of memory while building the tree");
            node_destroy(node);
            return NULL;
        }

        if (read_directory_entries(ctx, target_path, false, &entries, &count) != 0) {
            free(target_path);
            node_destroy(node);
            return NULL;
        }

        free(target_path);

        for (i = 0; i < count; ++i) {
            char *child_rel = relative_join(relative_path, entries[i].name);
            Node *child;

            if (child_rel == NULL) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(entries, count);
                node_destroy(node);
                return NULL;
            }

            child = build_target_only_node(ctx, child_rel, entries[i].name, entries[i].kind, node);
            free(child_rel);
            if (child == NULL) {
                free_dir_entries(entries, count);
                node_destroy(node);
                return NULL;
            }

            if (node_add_child(node, child) != 0) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(entries, count);
                node_destroy(node);
                return NULL;
            }
        }

        qsort(node->children, node->child_count, sizeof(*node->children), node_compare);
        free_dir_entries(entries, count);
    }

    return node;
}

static Node *build_source_node(
    BuildContext *ctx,
    const char *relative_path,
    const char *name,
    NodeKind kind,
    Node *parent
) {
    Node *node = node_create(name, relative_path, kind, ORIGIN_SOURCE, parent);
    char *target_path = NULL;
    NodeKind target_kind = NODE_FILE;
    bool target_exists = false;

    if (node == NULL) {
        set_error(ctx, "Out of memory while building the tree");
        return NULL;
    }

    target_path = path_join(ctx->target_root, relative_path);
    if (target_path == NULL) {
        set_error(ctx, "Out of memory while building the tree");
        node_destroy(node);
        return NULL;
    }

    if (classify_target_path(target_path, &target_kind, &target_exists) != 0) {
        set_error(ctx, "Cannot inspect '%s': %s", target_path, strerror(errno));
        free(target_path);
        node_destroy(node);
        return NULL;
    }

    if (target_exists) {
        if (target_kind == kind) {
            node->target_match = true;
        } else {
            node->target_conflict = true;
            node->conflict_kind = target_kind;
            node->conflict_target_item_count = count_target_items_recursive(ctx, target_path);
            if (ctx->err[0] != '\0') {
                free(target_path);
                node_destroy(node);
                return NULL;
            }
        }
    }

    if (kind == NODE_DIR) {
        char *source_path = path_join(ctx->source_root, relative_path);
        DirEntry *source_entries = NULL;
        DirEntry *target_entries = NULL;
        size_t source_count = 0;
        size_t target_count = 0;
        size_t i;

        if (source_path == NULL) {
            set_error(ctx, "Out of memory while building the tree");
            free(target_path);
            node_destroy(node);
            return NULL;
        }

        if (read_directory_entries(ctx, source_path, true, &source_entries, &source_count) != 0) {
            free(source_path);
            free(target_path);
            node_destroy(node);
            return NULL;
        }

        if (node->target_match) {
            if (read_directory_entries(ctx, target_path, false, &target_entries, &target_count) != 0) {
                free(source_path);
                free(target_path);
                free_dir_entries(source_entries, source_count);
                node_destroy(node);
                return NULL;
            }
        }

        free(source_path);
        free(target_path);
        target_path = NULL;

        for (i = 0; i < source_count; ++i) {
            char *child_rel = relative_join(relative_path, source_entries[i].name);
            Node *child;

            if (child_rel == NULL) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }

            child = build_source_node(ctx, child_rel, source_entries[i].name, source_entries[i].kind, node);
            free(child_rel);
            if (child == NULL) {
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }

            if (node_add_child(node, child) != 0) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }
        }

        for (i = 0; i < target_count; ++i) {
            char *child_rel;
            Node *child;

            if (name_exists(source_entries, source_count, target_entries[i].name)) {
                continue;
            }

            child_rel = relative_join(relative_path, target_entries[i].name);
            if (child_rel == NULL) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }

            child = build_target_only_node(ctx, child_rel, target_entries[i].name, target_entries[i].kind, node);
            free(child_rel);
            if (child == NULL) {
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }

            if (node_add_child(node, child) != 0) {
                set_error(ctx, "Out of memory while building the tree");
                free_dir_entries(source_entries, source_count);
                free_dir_entries(target_entries, target_count);
                node_destroy(node);
                return NULL;
            }
        }

        qsort(node->children, node->child_count, sizeof(*node->children), node_compare);
        free_dir_entries(source_entries, source_count);
        free_dir_entries(target_entries, target_count);
    } else {
        free(target_path);
    }

    return node;
}

static size_t recompute_node(Node *node) {
    size_t i;

    if (node->origin == ORIGIN_TARGET_ONLY) {
        size_t total = 1;
        for (i = 0; i < node->child_count; ++i) {
            total += recompute_node(node->children[i]);
        }
        return total;
    }

    node->checked = node->target_match;
    node->has_overlap = node->target_match;
    node->hidden_target_only_count = 0;
    node->replacement_delete_count = node->target_conflict ? node->conflict_target_item_count : 0;

    if (node->kind == NODE_FILE) {
        return 0;
    }

    for (i = 0; i < node->child_count; ++i) {
        Node *child = node->children[i];
        size_t target_only_subtree = recompute_node(child);

        if (child->origin == ORIGIN_TARGET_ONLY) {
            node->hidden_target_only_count += target_only_subtree;
            continue;
        }

        if (!child->checked) {
            node->checked = false;
        }
        if (child->has_overlap) {
            node->has_overlap = true;
        }
        if (node->target_match) {
            node->hidden_target_only_count += child->hidden_target_only_count;
            node->replacement_delete_count += child->replacement_delete_count;
        }
    }

    return 0;
}

Tree *tree_build(const char *source_root, const char *target_root, char *err, size_t err_size) {
    BuildContext ctx = {
        .source_root = source_root,
        .target_root = target_root,
        .skipped_source_entries = 0,
        .skipped_target_entries = 0,
        .err = err,
        .err_size = err_size
    };
    Tree *tree;
    char *root_name;

    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    root_name = basename_copy(source_root);
    if (root_name == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "Out of memory while building the tree");
        }
        return NULL;
    }

    tree = calloc(1, sizeof(*tree));
    if (tree == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "Out of memory while building the tree");
        }
        free(root_name);
        return NULL;
    }

    tree->root = build_source_node(&ctx, "", root_name, NODE_DIR, NULL);
    free(root_name);
    if (tree->root == NULL) {
        tree_free(tree);
        return NULL;
    }

    tree->root->expanded = true;
    recompute_node(tree->root);
    tree->skipped_source_entries = ctx.skipped_source_entries;
    tree->skipped_target_entries = ctx.skipped_target_entries;
    return tree;
}

Node *tree_find_source_node(Node *root, const char *relative_path) {
    size_t i;

    if (root == NULL) {
        return NULL;
    }

    if (root->origin == ORIGIN_SOURCE && strcmp(root->relative_path, relative_path) == 0) {
        return root;
    }

    for (i = 0; i < root->child_count; ++i) {
        Node *match = tree_find_source_node(root->children[i], relative_path);
        if (match != NULL) {
            return match;
        }
    }

    return NULL;
}

static int append_visible_row(VisibleRow **rows, size_t *count, size_t *capacity, Node *node, int depth) {
    VisibleRow *grown;

    if (*count == *capacity) {
        size_t next_capacity = *capacity == 0 ? 32 : (*capacity * 2);
        grown = realloc(*rows, next_capacity * sizeof(**rows));
        if (grown == NULL) {
            return -1;
        }
        *rows = grown;
        *capacity = next_capacity;
    }

    (*rows)[*count].node = node;
    (*rows)[*count].depth = depth;
    ++(*count);
    return 0;
}

static int collect_visible_recursive(
    Node *node,
    int depth,
    bool show_target_only,
    VisibleRow **rows,
    size_t *count,
    size_t *capacity
) {
    size_t i;

    if (node->origin == ORIGIN_TARGET_ONLY && !show_target_only) {
        return 0;
    }

    if (append_visible_row(rows, count, capacity, node, depth) != 0) {
        return -1;
    }

    if (node->kind != NODE_DIR || !node->expanded) {
        return 0;
    }

    for (i = 0; i < node->child_count; ++i) {
        if (collect_visible_recursive(node->children[i], depth + 1, show_target_only, rows, count, capacity) != 0) {
            return -1;
        }
    }

    return 0;
}

size_t tree_collect_visible(Node *root, bool show_target_only, VisibleRow **out_rows) {
    VisibleRow *rows = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i;

    *out_rows = NULL;

    if (root == NULL) {
        return 0;
    }

    for (i = 0; i < root->child_count; ++i) {
        if (collect_visible_recursive(root->children[i], 0, show_target_only, &rows, &count, &capacity) != 0) {
            free(rows);
            return 0;
        }
    }

    *out_rows = rows;
    return count;
}
