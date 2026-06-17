/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef SUBSYNC_H
#define SUBSYNC_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    NODE_FILE,
    NODE_DIR
} NodeKind;

typedef enum {
    ORIGIN_SOURCE,
    ORIGIN_TARGET_ONLY
} NodeOrigin;

typedef struct Node Node;

struct Node {
    char *name;
    char *relative_path;
    NodeKind kind;
    NodeOrigin origin;
    bool target_match;
    bool target_conflict;
    NodeKind conflict_kind;
    size_t conflict_target_item_count;
    bool checked;
    bool has_overlap;
    bool expanded;
    size_t hidden_target_only_count;
    size_t replacement_delete_count;
    Node *parent;
    Node **children;
    size_t child_count;
};

typedef struct {
    Node *root;
    size_t skipped_source_entries;
    size_t skipped_target_entries;
} Tree;

typedef struct {
    Node *node;
    int depth;
} VisibleRow;

typedef enum {
    TREE_PROGRESS_SOURCE,
    TREE_PROGRESS_TARGET
} TreeBuildPhase;

typedef struct {
    TreeBuildPhase phase;
    const char *current_path;
    size_t directories_discovered;
    size_t directories_completed;
    size_t source_entries_scanned;
    size_t target_entries_scanned;
    size_t nodes_created;
} TreeBuildProgress;

typedef void (*TreeBuildProgressFn)(const TreeBuildProgress *progress, void *userdata);

typedef struct {
    const char *current_path;
    size_t files_completed;
    unsigned long long bytes_completed;
    unsigned long long current_file_bytes;
    unsigned long long current_file_total;
} TransferProgress;

typedef void (*TransferProgressFn)(const TransferProgress *progress, void *userdata);

Tree *tree_build(
    const char *source_root,
    const char *target_root,
    char *err,
    size_t err_size,
    TreeBuildProgressFn progress_fn,
    void *progress_userdata
);
void tree_free(Tree *tree);
Node *tree_find_source_node(Node *root, const char *relative_path);
size_t tree_collect_visible(Node *root, bool show_target_only, VisibleRow **out_rows);
bool subsync_target_uses_gio(const char *path);

int copy_source_node_to_target(
    const char *source_root,
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size,
    TransferProgressFn progress_fn,
    void *progress_userdata
);
int remove_target_for_source_node(
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size,
    TransferProgressFn progress_fn,
    void *progress_userdata
);

int ui_run(const char *source_root, const char *target_root);

#endif
