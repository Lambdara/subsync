/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "subsync.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *err, size_t err_size, const char *fmt, ...) {
    va_list args;

    if (err == NULL || err_size == 0) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

static char *dup_string(const char *value) {
    size_t len;
    char *copy;

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

static int ensure_directory_recursive(const char *path, char *err, size_t err_size) {
    struct stat st;
    char *parent;
    char *slash;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        set_error(err, err_size, "'%s' exists and is not a directory", path);
        return -1;
    }

    if (errno != ENOENT) {
        set_error(err, err_size, "Cannot inspect '%s': %s", path, strerror(errno));
        return -1;
    }

    parent = dup_string(path);
    if (parent == NULL) {
        set_error(err, err_size, "Out of memory while preparing '%s'", path);
        return -1;
    }

    slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        if (ensure_directory_recursive(parent, err, err_size) != 0) {
            free(parent);
            return -1;
        }
    } else if (slash == parent) {
        parent[1] = '\0';
        if (ensure_directory_recursive(parent, err, err_size) != 0) {
            free(parent);
            return -1;
        }
    }

    free(parent);

    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        set_error(err, err_size, "Cannot create directory '%s': %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int ensure_parent_directory(const char *path, char *err, size_t err_size) {
    char *parent = dup_string(path);
    char *slash;
    int rc = 0;

    if (parent == NULL) {
        set_error(err, err_size, "Out of memory while preparing '%s'", path);
        return -1;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        free(parent);
        return 0;
    }

    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }

    rc = ensure_directory_recursive(parent, err, err_size);
    free(parent);
    return rc;
}

static int remove_path_recursive(const char *path, char *err, size_t err_size) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return 0;
        }
        set_error(err, err_size, "Cannot inspect '%s': %s", path, strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            set_error(err, err_size, "Cannot remove '%s': %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }

    DIR *dir = opendir(path);
    struct dirent *entry;
    int saved_errno = 0;

    if (dir == NULL) {
        set_error(err, err_size, "Cannot open '%s': %s", path, strerror(errno));
        return -1;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *child_path;
        int rc;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        child_path = path_join(path, entry->d_name);
        if (child_path == NULL) {
            closedir(dir);
            set_error(err, err_size, "Out of memory while removing '%s'", path);
            return -1;
        }

        rc = remove_path_recursive(child_path, err, err_size);
        free(child_path);
        if (rc != 0) {
            closedir(dir);
            return -1;
        }
    }

    saved_errno = errno;
    closedir(dir);
    if (saved_errno != 0) {
        set_error(err, err_size, "Cannot read '%s': %s", path, strerror(saved_errno));
        return -1;
    }

    if (rmdir(path) != 0) {
        set_error(err, err_size, "Cannot remove directory '%s': %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int prepare_target_path(const char *target_path, NodeKind expected_kind, char *err, size_t err_size) {
    struct stat st;

    if (lstat(target_path, &st) == 0) {
        if ((expected_kind == NODE_DIR && S_ISDIR(st.st_mode)) ||
            (expected_kind == NODE_FILE && !S_ISDIR(st.st_mode))) {
            return 0;
        }
        if (remove_path_recursive(target_path, err, err_size) != 0) {
            return -1;
        }
    } else if (errno != ENOENT && errno != ENOTDIR) {
        set_error(err, err_size, "Cannot inspect '%s': %s", target_path, strerror(errno));
        return -1;
    }

    if (expected_kind == NODE_DIR) {
        return ensure_directory_recursive(target_path, err, err_size);
    }
    return ensure_parent_directory(target_path, err, err_size);
}

static int copy_file_contents(const char *source_path, const char *target_path, char *err, size_t err_size) {
    int source_fd = -1;
    int target_fd = -1;
    struct stat st;
    char buffer[65536];
    ssize_t bytes_read;

    if (ensure_parent_directory(target_path, err, err_size) != 0) {
        return -1;
    }

    source_fd = open(source_path, O_RDONLY);
    if (source_fd < 0) {
        set_error(err, err_size, "Cannot open '%s': %s", source_path, strerror(errno));
        return -1;
    }

    if (fstat(source_fd, &st) != 0) {
        set_error(err, err_size, "Cannot inspect '%s': %s", source_path, strerror(errno));
        close(source_fd);
        return -1;
    }

    target_fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (target_fd < 0) {
        set_error(err, err_size, "Cannot create '%s': %s", target_path, strerror(errno));
        close(source_fd);
        return -1;
    }

    while ((bytes_read = read(source_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;

        while (total_written < bytes_read) {
            ssize_t bytes_written = write(target_fd, buffer + total_written, (size_t)(bytes_read - total_written));
            if (bytes_written < 0) {
                set_error(err, err_size, "Cannot write '%s': %s", target_path, strerror(errno));
                close(source_fd);
                close(target_fd);
                return -1;
            }
            total_written += bytes_written;
        }
    }

    if (bytes_read < 0) {
        set_error(err, err_size, "Cannot read '%s': %s", source_path, strerror(errno));
        close(source_fd);
        close(target_fd);
        return -1;
    }

    close(source_fd);
    close(target_fd);
    return 0;
}

static int copy_source_node_recursive(
    const char *source_root,
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size
) {
    char *source_path;
    char *target_path;
    size_t i;

    if (node->origin != ORIGIN_SOURCE) {
        return 0;
    }

    source_path = path_join(source_root, node->relative_path);
    target_path = path_join(target_root, node->relative_path);
    if (source_path == NULL || target_path == NULL) {
        free(source_path);
        free(target_path);
        set_error(err, err_size, "Out of memory while copying '%s'", node->name);
        return -1;
    }

    if (node->kind == NODE_FILE) {
        if (node->target_match) {
            free(source_path);
            free(target_path);
            return 0;
        }

        if (prepare_target_path(target_path, NODE_FILE, err, err_size) != 0) {
            free(source_path);
            free(target_path);
            return -1;
        }

        if (copy_file_contents(source_path, target_path, err, err_size) != 0) {
            free(source_path);
            free(target_path);
            return -1;
        }

        free(source_path);
        free(target_path);
        return 0;
    }

    if (!node->target_match) {
        if (prepare_target_path(target_path, NODE_DIR, err, err_size) != 0) {
            free(source_path);
            free(target_path);
            return -1;
        }
    }

    free(source_path);
    free(target_path);

    for (i = 0; i < node->child_count; ++i) {
        if (node->children[i]->origin != ORIGIN_SOURCE) {
            continue;
        }
        if (copy_source_node_recursive(source_root, target_root, node->children[i], err, err_size) != 0) {
            return -1;
        }
    }

    return 0;
}

int copy_source_node_to_target(
    const char *source_root,
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size
) {
    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        set_error(err, err_size, "No source item is selected");
        return -1;
    }

    return copy_source_node_recursive(source_root, target_root, node, err, err_size);
}

int remove_target_for_source_node(
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size
) {
    char *target_path;
    int rc;

    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        set_error(err, err_size, "No source item is selected");
        return -1;
    }

    target_path = path_join(target_root, node->relative_path);
    if (target_path == NULL) {
        set_error(err, err_size, "Out of memory while removing '%s'", node->name);
        return -1;
    }

    rc = remove_path_recursive(target_path, err, err_size);
    free(target_path);
    return rc;
}
