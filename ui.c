/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "subsync.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    JOB_COPY,
    JOB_REMOVE
} JobAction;

typedef enum {
    JOB_PENDING,
    JOB_RUNNING
} JobState;

typedef struct {
    pid_t pid;
    int result_fd;
    char *relative_path;
    JobAction action;
    JobState state;
    size_t file_count;
    unsigned long long byte_count;
} TransferJob;

typedef struct {
    char *source_root;
    char *target_root;
    Tree *tree;
    bool show_target_only;
    size_t selected_index;
    size_t scroll_offset;
    TransferJob *jobs;
    size_t job_count;
    size_t job_capacity;
    char status[512];
} UiState;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

static int reload_tree(UiState *ui, const char *selected_path);

static void set_status(UiState *ui, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(ui->status, sizeof(ui->status), fmt, args);
    va_end(args);
}

static char *dup_string(const char *value) {
    size_t len = strlen(value);
    char *copy = malloc(len + 1);

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

static bool path_is_same_or_nested(const char *left, const char *right) {
    size_t left_len;

    if (left == NULL || right == NULL) {
        return false;
    }

    left_len = strlen(left);
    if (left_len == 0) {
        return true;
    }

    if (strncmp(left, right, left_len) != 0) {
        return false;
    }

    return right[left_len] == '\0' || right[left_len] == '/';
}

static bool paths_overlap(const char *left, const char *right) {
    return path_is_same_or_nested(left, right) || path_is_same_or_nested(right, left);
}

static const char *action_label(JobAction action) {
    return action == JOB_COPY ? "copy" : "remove";
}

static const char *job_state_label(JobState state) {
    return state == JOB_RUNNING ? "running" : "queued";
}

static void free_job(TransferJob *job) {
    if (job == NULL) {
        return;
    }

    if (job->result_fd >= 0) {
        close(job->result_fd);
    }
    free(job->relative_path);
}

static void free_jobs(UiState *ui) {
    size_t i;

    for (i = 0; i < ui->job_count; ++i) {
        free_job(&ui->jobs[i]);
    }
    free(ui->jobs);
    ui->jobs = NULL;
    ui->job_count = 0;
    ui->job_capacity = 0;
}

static bool has_running_job(const UiState *ui) {
    size_t i;

    for (i = 0; i < ui->job_count; ++i) {
        if (ui->jobs[i].state == JOB_RUNNING) {
            return true;
        }
    }

    return false;
}

static void queue_totals(
    const UiState *ui,
    size_t *out_jobs,
    size_t *out_files,
    unsigned long long *out_bytes,
    size_t *out_running,
    size_t *out_pending
) {
    size_t i;
    size_t jobs = 0;
    size_t files = 0;
    unsigned long long bytes = 0;
    size_t running = 0;
    size_t pending = 0;

    for (i = 0; i < ui->job_count; ++i) {
        ++jobs;
        files += ui->jobs[i].file_count;
        bytes += ui->jobs[i].byte_count;
        if (ui->jobs[i].state == JOB_RUNNING) {
            ++running;
        } else {
            ++pending;
        }
    }

    *out_jobs = jobs;
    *out_files = files;
    *out_bytes = bytes;
    *out_running = running;
    *out_pending = pending;
}

static int summarize_path_recursive(
    const char *path,
    size_t *out_files,
    unsigned long long *out_bytes,
    char *err,
    size_t err_size
) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            *out_files = 0;
            *out_bytes = 0;
            return 0;
        }
        snprintf(err, err_size, "Cannot inspect '%s': %s", path, strerror(errno));
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        *out_files = 1;
        *out_bytes = (unsigned long long)st.st_size;
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        *out_files = 0;
        *out_bytes = 0;
        return 0;
    }

    DIR *dir = opendir(path);
    struct dirent *entry;
    size_t files = 0;
    unsigned long long bytes = 0;
    int saved_errno = 0;

    if (dir == NULL) {
        snprintf(err, err_size, "Cannot open '%s': %s", path, strerror(errno));
        return -1;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *child_path;
        size_t child_files = 0;
        unsigned long long child_bytes = 0;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        child_path = path_join(path, entry->d_name);
        if (child_path == NULL) {
            closedir(dir);
            snprintf(err, err_size, "Out of memory while scanning '%s'", path);
            return -1;
        }

        if (summarize_path_recursive(child_path, &child_files, &child_bytes, err, err_size) != 0) {
            free(child_path);
            closedir(dir);
            return -1;
        }

        free(child_path);
        files += child_files;
        bytes += child_bytes;
    }

    saved_errno = errno;
    closedir(dir);
    if (saved_errno != 0) {
        snprintf(err, err_size, "Cannot read '%s': %s", path, strerror(saved_errno));
        return -1;
    }

    *out_files = files;
    *out_bytes = bytes;
    return 0;
}

static int summarize_job_workload(
    UiState *ui,
    const Node *node,
    JobAction action,
    size_t *out_files,
    unsigned long long *out_bytes
) {
    char *path;
    char err[512];
    int rc;

    path = path_join(action == JOB_COPY ? ui->source_root : ui->target_root, node->relative_path);
    if (path == NULL) {
        set_status(ui, "Out of memory while preparing the queue entry.");
        return -1;
    }

    err[0] = '\0';
    rc = summarize_path_recursive(path, out_files, out_bytes, err, sizeof(err));
    free(path);
    if (rc != 0) {
        set_status(ui, "%s", err[0] != '\0' ? err : "Failed to inspect the queued workload.");
        return -1;
    }

    return 0;
}

static int append_job(UiState *ui, TransferJob *job) {
    TransferJob *grown;

    if (ui->job_count == ui->job_capacity) {
        size_t next_capacity = ui->job_capacity == 0 ? 8 : ui->job_capacity * 2;
        grown = realloc(ui->jobs, next_capacity * sizeof(*ui->jobs));
        if (grown == NULL) {
            return -1;
        }
        ui->jobs = grown;
        ui->job_capacity = next_capacity;
    }

    ui->jobs[ui->job_count] = *job;
    ++ui->job_count;
    return 0;
}

static bool set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0) {
        return false;
    }

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool job_overlaps_path(const TransferJob *job, const char *relative_path) {
    return paths_overlap(job->relative_path, relative_path);
}

static const TransferJob *find_overlapping_job(const UiState *ui, const char *relative_path) {
    size_t i;

    for (i = 0; i < ui->job_count; ++i) {
        if (job_overlaps_path(&ui->jobs[i], relative_path)) {
            return &ui->jobs[i];
        }
    }

    return NULL;
}

static bool node_has_pending_job(const UiState *ui, const Node *node) {
    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        return false;
    }

    return find_overlapping_job(ui, node->relative_path) != NULL;
}

static int read_fd_message(int fd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    if (buffer_size == 0) {
        return -1;
    }

    while (used + 1 < buffer_size) {
        ssize_t bytes_read = read(fd, buffer + used, buffer_size - used - 1);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }

    buffer[used] = '\0';
    return 0;
}

static void write_child_message(int fd, const char *message) {
    size_t len = strlen(message);
    size_t used = 0;

    while (used < len) {
        ssize_t bytes_written = write(fd, message + used, len - used);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        used += (size_t)bytes_written;
    }
}

static void run_transfer_child(UiState *ui, const Node *node, JobAction action, int result_fd) {
    char message[512];
    char err[512];
    int rc;

    err[0] = '\0';
    if (action == JOB_COPY) {
        rc = copy_source_node_to_target(ui->source_root, ui->target_root, node, err, sizeof(err));
        if (rc == 0) {
            snprintf(
                message,
                sizeof(message),
                "Copied '%s' to the target.",
                node->relative_path[0] != '\0' ? node->relative_path : "."
            );
        }
    } else {
        rc = remove_target_for_source_node(ui->target_root, node, err, sizeof(err));
        if (rc == 0) {
            snprintf(
                message,
                sizeof(message),
                "Removed '%s' from the target.",
                node->relative_path[0] != '\0' ? node->relative_path : "."
            );
        }
    }

    if (rc != 0) {
        snprintf(
            message,
            sizeof(message),
            "%s",
            err[0] != '\0' ? err : (action == JOB_COPY ? "Failed to copy the source item." : "Failed to remove the target item.")
        );
    }

    write_child_message(result_fd, message);
    close(result_fd);
    _exit(rc == 0 ? 0 : 1);
}

static int remove_job_at(UiState *ui, size_t index) {
    if (index >= ui->job_count) {
        return -1;
    }

    free_job(&ui->jobs[index]);
    if (index + 1 < ui->job_count) {
        memmove(&ui->jobs[index], &ui->jobs[index + 1], (ui->job_count - index - 1) * sizeof(*ui->jobs));
    }
    --ui->job_count;
    return 0;
}

static int spawn_job_process(UiState *ui, size_t index) {
    int pipe_fds[2];
    pid_t pid;
    Node *node;

    if (index >= ui->job_count) {
        set_status(ui, "Internal queue error: invalid job index.");
        return -1;
    }

    node = tree_find_source_node(ui->tree->root, ui->jobs[index].relative_path);
    if (node == NULL) {
        set_status(
            ui,
            "Queued %s for '%s' was dropped because the source item no longer exists.",
            action_label(ui->jobs[index].action),
            ui->jobs[index].relative_path[0] != '\0' ? ui->jobs[index].relative_path : "."
        );
        remove_job_at(ui, index);
        return 0;
    }

    if (pipe(pipe_fds) != 0) {
        set_status(ui, "Cannot create transfer pipe: %s", strerror(errno));
        return 0;
    }

    if (!set_close_on_exec(pipe_fds[0]) || !set_close_on_exec(pipe_fds[1])) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_status(ui, "Cannot prepare transfer pipe: %s", strerror(errno));
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_status(ui, "Cannot start transfer: %s", strerror(errno));
        return 0;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        run_transfer_child(ui, node, ui->jobs[index].action, pipe_fds[1]);
    }

    close(pipe_fds[1]);
    ui->jobs[index].pid = pid;
    ui->jobs[index].result_fd = pipe_fds[0];
    ui->jobs[index].state = JOB_RUNNING;

    set_status(
        ui,
        "Started %s for '%s'.",
        action_label(ui->jobs[index].action),
        ui->jobs[index].relative_path[0] != '\0' ? ui->jobs[index].relative_path : "."
    );
    return 0;
}

static int start_next_pending_job(UiState *ui) {
    size_t i;

    if (has_running_job(ui)) {
        return 0;
    }

    for (i = 0; i < ui->job_count; ++i) {
        if (ui->jobs[i].state == JOB_PENDING) {
            return spawn_job_process(ui, i);
        }
    }

    return 0;
}

static int finish_completed_jobs(UiState *ui, const char *selected_path) {
    size_t i;

    for (i = 0; i < ui->job_count; ++i) {
        int status;
        pid_t rc;
        char last_message[512] = "";

        if (ui->jobs[i].state != JOB_RUNNING) {
            continue;
        }

        rc = waitpid(ui->jobs[i].pid, &status, WNOHANG);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            set_status(ui, "Cannot wait for transfer %ld: %s", (long)ui->jobs[i].pid, strerror(errno));
            return -1;
        }

        if (read_fd_message(ui->jobs[i].result_fd, last_message, sizeof(last_message)) != 0) {
            snprintf(last_message, sizeof(last_message), "Transfer for '%s' finished without a readable result.", ui->jobs[i].relative_path);
        }

        close(ui->jobs[i].result_fd);
        ui->jobs[i].result_fd = -1;

        set_status(ui, "%s", last_message[0] != '\0' ? last_message : "Transfer finished.");
        remove_job_at(ui, i);

        if (reload_tree(ui, selected_path) != 0) {
            return -1;
        }

        if (start_next_pending_job(ui) != 0) {
            return -1;
        }

        return 1;
    }

    return start_next_pending_job(ui);
}

static void string_list_free(StringList *list) {
    size_t i;

    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int string_list_append(StringList *list, const char *value) {
    char **grown;
    char *copy;

    if (list->count == list->capacity) {
        size_t next_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        grown = realloc(list->items, next_capacity * sizeof(*list->items));
        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->capacity = next_capacity;
    }

    copy = dup_string(value);
    if (copy == NULL) {
        return -1;
    }

    list->items[list->count] = copy;
    ++list->count;
    return 0;
}

static bool string_list_contains(const StringList *list, const char *value) {
    size_t i;

    for (i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static void collect_expanded_paths(Node *node, StringList *list) {
    size_t i;

    if (node == NULL) {
        return;
    }

    if (node->origin == ORIGIN_SOURCE && node->kind == NODE_DIR && node->expanded && node->relative_path[0] != '\0') {
        if (string_list_append(list, node->relative_path) != 0) {
            return;
        }
    }

    for (i = 0; i < node->child_count; ++i) {
        collect_expanded_paths(node->children[i], list);
    }
}

static void apply_expanded_paths(Node *node, const StringList *list) {
    size_t i;

    if (node == NULL) {
        return;
    }

    if (node->kind == NODE_DIR && node->relative_path[0] != '\0' &&
        string_list_contains(list, node->relative_path)) {
        node->expanded = true;
    }

    for (i = 0; i < node->child_count; ++i) {
        apply_expanded_paths(node->children[i], list);
    }
}

static size_t find_index_for_path(VisibleRow *rows, size_t count, const char *relative_path) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rows[i].node->origin == ORIGIN_SOURCE &&
            strcmp(rows[i].node->relative_path, relative_path) == 0) {
            return i;
        }
    }
    return 0;
}

static int reload_tree(UiState *ui, const char *selected_path) {
    char err[512];
    StringList expanded = {0};
    Tree *next_tree;

    if (ui->tree != NULL) {
        collect_expanded_paths(ui->tree->root, &expanded);
    }

    err[0] = '\0';
    next_tree = tree_build(ui->source_root, ui->target_root, err, sizeof(err));
    if (next_tree == NULL) {
        string_list_free(&expanded);
        snprintf(ui->status, sizeof(ui->status), "%s", err[0] != '\0' ? err : "Failed to reload the tree");
        return -1;
    }

    apply_expanded_paths(next_tree->root, &expanded);
    next_tree->root->expanded = true;

    tree_free(ui->tree);
    ui->tree = next_tree;

    if (next_tree->skipped_source_entries > 0 && ui->status[0] == '\0') {
        set_status(ui, "Skipped %zu unsupported source entries.", next_tree->skipped_source_entries);
    }

    if (selected_path != NULL) {
        VisibleRow *rows = NULL;
        size_t count = tree_collect_visible(next_tree->root, ui->show_target_only, &rows);
        if (rows != NULL && count > 0) {
            ui->selected_index = find_index_for_path(rows, count, selected_path);
        } else {
            ui->selected_index = 0;
        }
        free(rows);
    } else {
        ui->selected_index = 0;
    }

    string_list_free(&expanded);
    return 0;
}

static int find_row_index_for_node(VisibleRow *rows, size_t count, const Node *node) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rows[i].node == node) {
            return (int)i;
        }
    }
    return -1;
}

static void clamp_scroll(UiState *ui, size_t row_count, int list_height) {
    if (list_height <= 0) {
        ui->scroll_offset = 0;
        return;
    }

    if (ui->selected_index < ui->scroll_offset) {
        ui->scroll_offset = ui->selected_index;
    }

    if (ui->selected_index >= ui->scroll_offset + (size_t)list_height) {
        ui->scroll_offset = ui->selected_index - (size_t)list_height + 1;
    }

    if (ui->scroll_offset > row_count) {
        ui->scroll_offset = row_count == 0 ? 0 : row_count - 1;
    }
}

static char status_marker(const Node *node) {
    if (node->origin == ORIGIN_TARGET_ONLY) {
        return ' ';
    }
    if ((node->checked && node->hidden_target_only_count > 0) ||
        (!node->checked && node->replacement_delete_count > 0)) {
        return '!';
    }
    if (!node->checked && node->has_overlap) {
        return '~';
    }
    return ' ';
}

static char pending_marker(const UiState *ui, const Node *node) {
    return node_has_pending_job(ui, node) ? '*' : ' ';
}

static void format_queue_summary(const UiState *ui, char *buffer, size_t buffer_size) {
    size_t jobs;
    size_t files;
    unsigned long long bytes;
    size_t running;
    size_t pending;

    queue_totals(ui, &jobs, &files, &bytes, &running, &pending);
    snprintf(
        buffer,
        buffer_size,
        "Queue: %zu file(s) / %.1f MB",
        files,
        (double)bytes / (1024.0 * 1024.0)
    );
}

static void format_row(char *buffer, size_t buffer_size, UiState *ui, const VisibleRow *row) {
    const Node *node = row->node;
    int indent = row->depth * 2;
    char expand = node->kind == NODE_DIR ? (node->expanded ? 'v' : '>') : ' ';
    const char *suffix = node->kind == NODE_DIR ? "/" : "";

    if (node->origin == ORIGIN_TARGET_ONLY) {
        snprintf(
            buffer,
            buffer_size,
            "%*s%c [target]%c %s%s",
            indent,
            "",
            expand,
            pending_marker(ui, node),
            node->name,
            suffix
        );
        return;
    }

    snprintf(
        buffer,
        buffer_size,
        "%*s%c [%c]%c%c %s%s",
        indent,
        "",
        expand,
        node->checked ? 'x' : ' ',
        status_marker(node),
        pending_marker(ui, node),
        node->name,
        suffix
    );
}

static void render_selected_summary(const UiState *ui, const Node *node) {
    const TransferJob *job;

    if (node == NULL) {
        mvaddnstr(LINES - 1, 0, "No items in the source directory.", COLS - 1);
        return;
    }

    job = find_overlapping_job(ui, node->relative_path);
    if (job != NULL) {
        if (job->state == JOB_RUNNING) {
            mvprintw(
                LINES - 1,
                0,
                "Transfer running for '%s'. Wait for it to finish before changing overlapping paths.",
                job->relative_path[0] != '\0' ? job->relative_path : "."
            );
        } else {
            mvprintw(
                LINES - 1,
                0,
                "Transfer queued for '%s'. Wait for earlier queued work to clear before changing overlapping paths.",
                job->relative_path[0] != '\0' ? job->relative_path : "."
            );
        }
        clrtoeol();
        return;
    }

    if (node->origin == ORIGIN_TARGET_ONLY) {
        mvprintw(
            LINES - 1,
            0,
            "Target-only item: %s. It is visible for inspection only.",
            node->relative_path
        );
        clrtoeol();
        return;
    }

    if (node->checked) {
        if (node->kind == NODE_DIR && node->hidden_target_only_count > 0) {
            mvprintw(
                LINES - 1,
                0,
                "%s is fully present on the target. Delete removes it and %zu extra target item(s).",
                node->relative_path[0] != '\0' ? node->relative_path : ".",
                node->hidden_target_only_count
            );
        } else {
            mvprintw(
                LINES - 1,
                0,
                "%s is present on the target. Delete removes it from the target.",
                node->relative_path[0] != '\0' ? node->relative_path : "."
            );
        }
        clrtoeol();
        return;
    }

    if (node->replacement_delete_count > 0) {
        mvprintw(
            LINES - 1,
            0,
            "%s is not fully present. Enter queues a copy and deletes %zu conflicting target item(s).",
            node->relative_path[0] != '\0' ? node->relative_path : ".",
            node->replacement_delete_count
        );
        clrtoeol();
        return;
    }

    if (node->kind == NODE_DIR && node->has_overlap) {
        mvprintw(
            LINES - 1,
            0,
            "%s is partially present on the target. Enter queues the missing source items.",
            node->relative_path[0] != '\0' ? node->relative_path : "."
        );
        clrtoeol();
        return;
    }

    mvprintw(
        LINES - 1,
        0,
        "%s is missing on the target. Enter queues it for copy to the matching target path.",
        node->relative_path[0] != '\0' ? node->relative_path : "."
    );
    clrtoeol();
}

static void draw_popup(const char *title, const char *const *lines, size_t line_count, bool wait_for_key) {
    int width = 40;
    int height = (int)line_count + 4;
    size_t i;
    WINDOW *win;

    for (i = 0; i < line_count; ++i) {
        int line_len = (int)strlen(lines[i]) + 4;
        if (line_len > width) {
            width = line_len;
        }
    }

    if (width > COLS - 2) {
        width = COLS - 2;
    }
    if (height > LINES - 2) {
        height = LINES - 2;
    }

    win = newwin(height, width, (LINES - height) / 2, (COLS - width) / 2);
    box(win, 0, 0);
    mvwaddnstr(win, 0, 2, title, width - 4);
    for (i = 0; i < line_count && (int)i + 1 < height - 1; ++i) {
        mvwaddnstr(win, (int)i + 1, 2, lines[i], width - 4);
    }
    wrefresh(win);

    if (wait_for_key) {
        wtimeout(win, -1);
        wgetch(win);
    }

    delwin(win);
}

static bool confirm_destructive_action(const char *verb, size_t count) {
    char line1[128];
    char line2[128];
    const char *lines[3];
    int ch;

    snprintf(line1, sizeof(line1), "%s this node will delete %zu target item(s)", verb, count);
    snprintf(line2, sizeof(line2), "that are not present at the source.");
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = "Continue? [y/N]";

    draw_popup("Warning", lines, 3, false);
    timeout(-1);
    ch = getch();
    timeout(100);
    return ch == 'y' || ch == 'Y';
}

static void show_help(void) {
    const char *lines[] = {
        "Up/Down  Move through the visible list",
        "Right    Expand the selected directory",
        "Left     Collapse it, or move to the parent",
        "Enter    Check the selected source item and queue a copy",
        "Delete   Uncheck the selected source item and queue removal",
        "a        Show or hide target-only items",
        "r        Reload the source and target directories",
        "q        Quit; asks if the queue is not empty",
        "",
        "[x]      Present on target; directories mean the full source subtree exists",
        "[ ]      Missing on target",
        "~        Some of the source subtree is already present on the target",
        "!        Toggling this node deletes or replaces target-only target content",
        "*        A transfer is queued or running for this item or subtree",
        "[target] Visible only when extras are shown; not interactable",
        "",
        "Press any key to return."
    };

    draw_popup("Controls", lines, sizeof(lines) / sizeof(lines[0]), true);
}

static void render(UiState *ui, VisibleRow *rows, size_t row_count) {
    size_t i;
    int list_top = 3;
    int list_height = LINES - 5;
    char status_line[512];
    char queue_summary[128];
    char source_line[1024];
    size_t queue_summary_len;
    int source_width;

    if (list_height < 0) {
        list_height = 0;
    }

    erase();
    format_queue_summary(ui, queue_summary, sizeof(queue_summary));
    queue_summary_len = strlen(queue_summary);
    snprintf(source_line, sizeof(source_line), "Source: %s", ui->source_root);
    source_width = COLS - (int)queue_summary_len - 1;
    if (source_width < 0) {
        source_width = 0;
    }
    mvaddnstr(0, 0, source_line, source_width);
    if ((int)queue_summary_len < COLS) {
        mvaddnstr(0, COLS - (int)queue_summary_len, queue_summary, (int)queue_summary_len);
    }
    clrtoeol();
    mvprintw(1, 0, "Target: %s", ui->target_root);
    clrtoeol();
    mvprintw(2, 0, "[?] help  [a] target-only: %s  [r] reload  [q] quit", ui->show_target_only ? "shown" : "hidden");
    clrtoeol();

    clamp_scroll(ui, row_count, list_height);

    for (i = 0; i < (size_t)list_height && ui->scroll_offset + i < row_count; ++i) {
        size_t row_index = ui->scroll_offset + i;
        char line[1024];

        format_row(line, sizeof(line), ui, &rows[row_index]);

        if (row_index == ui->selected_index) {
            attron(A_REVERSE);
        }
        mvaddnstr(list_top + (int)i, 0, line, COLS - 1);
        clrtoeol();
        if (row_index == ui->selected_index) {
            attroff(A_REVERSE);
        }
    }

    for (; i < (size_t)list_height; ++i) {
        move(list_top + (int)i, 0);
        clrtoeol();
    }

    if (ui->status[0] != '\0') {
        size_t jobs;
        size_t files;
        unsigned long long bytes;
        size_t running;
        size_t pending;

        queue_totals(ui, &jobs, &files, &bytes, &running, &pending);
        snprintf(
            status_line,
            sizeof(status_line),
            "Queue: %zu running, %zu waiting, %zu file(s), %.1f MB | %.420s",
            running,
            pending,
            files,
            (double)bytes / (1024.0 * 1024.0),
            ui->status
        );
    } else {
        size_t jobs;
        size_t files;
        unsigned long long bytes;
        size_t running;
        size_t pending;

        queue_totals(ui, &jobs, &files, &bytes, &running, &pending);
        snprintf(
            status_line,
            sizeof(status_line),
            "Queue: %zu running, %zu waiting, %zu file(s), %.1f MB",
            running,
            pending,
            files,
            (double)bytes / (1024.0 * 1024.0)
        );
    }
    mvaddnstr(LINES - 2, 0, status_line, COLS - 1);
    clrtoeol();

    render_selected_summary(ui, row_count > 0 ? rows[ui->selected_index].node : NULL);
    refresh();
}

static bool confirm_quit_with_queue(size_t count) {
    char line1[128];
    const char *lines[3];
    int ch;

    snprintf(line1, sizeof(line1), "%zu transfer(s) are still queued.", count);
    lines[0] = line1;
    lines[1] = "Quit anyway? The active transfer continues, but queued transfers are discarded.";
    lines[2] = "Continue? [y/N]";

    draw_popup("Quit", lines, 3, false);
    timeout(-1);
    ch = getch();
    timeout(100);
    return ch == 'y' || ch == 'Y';
}

static int queue_selected_node(UiState *ui, const Node *node, JobAction action) {
    const TransferJob *job;
    TransferJob queued_job;
    size_t jobs;
    size_t files;
    unsigned long long bytes;
    size_t running;
    size_t pending;

    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        set_status(ui, "Target-only items cannot be changed here.");
        return 0;
    }

    job = find_overlapping_job(ui, node->relative_path);
    if (job != NULL) {
        set_status(
            ui,
            "Cannot %s '%s' while '%s' is %s for an overlapping path.",
            action_label(action),
            node->relative_path[0] != '\0' ? node->relative_path : ".",
            job->relative_path[0] != '\0' ? job->relative_path : ".",
            job_state_label(job->state)
        );
        return 0;
    }

    if (action == JOB_REMOVE) {
        if (!node->checked) {
            set_status(
                ui,
                "'%s' is already unchecked. Use Enter to copy it to the target.",
                node->relative_path[0] != '\0' ? node->relative_path : "."
            );
            return 0;
        }

        if (node->kind == NODE_DIR && node->hidden_target_only_count > 0) {
            if (!confirm_destructive_action("Deleting", node->hidden_target_only_count)) {
                set_status(ui, "Operation cancelled.");
                return 0;
            }
        }
    }

    if (action == JOB_COPY && node->checked) {
        set_status(
            ui,
            "'%s' is already checked. Use Delete to remove it from the target.",
            node->relative_path[0] != '\0' ? node->relative_path : "."
        );
        return 0;
    }

    if (action == JOB_COPY && node->replacement_delete_count > 0) {
        if (!confirm_destructive_action("Copying", node->replacement_delete_count)) {
            set_status(ui, "Operation cancelled.");
            return 0;
        }
    }

    queued_job.pid = -1;
    queued_job.result_fd = -1;
    queued_job.relative_path = dup_string(node->relative_path);
    queued_job.action = action;
    queued_job.state = JOB_PENDING;
    queued_job.file_count = 0;
    queued_job.byte_count = 0;

    if (queued_job.relative_path == NULL) {
        set_status(ui, "Out of memory while queueing the transfer.");
        return 0;
    }

    if (summarize_job_workload(ui, node, action, &queued_job.file_count, &queued_job.byte_count) != 0) {
        free(queued_job.relative_path);
        return 0;
    }

    if (append_job(ui, &queued_job) != 0) {
        free(queued_job.relative_path);
        set_status(ui, "Out of memory while queueing the transfer.");
        return 0;
    }

    if (start_next_pending_job(ui) != 0) {
        return 0;
    }

    queue_totals(ui, &jobs, &files, &bytes, &running, &pending);
    set_status(
        ui,
        "%s '%s'. Queue: %zu job(s), %zu file(s), %.1f MB.",
        running > 0 && pending + running == 1 ? "Started" : "Queued",
        node->relative_path[0] != '\0' ? node->relative_path : ".",
        jobs,
        files,
        (double)bytes / (1024.0 * 1024.0)
    );
    return 0;
}

int ui_run(const char *source_root, const char *target_root) {
    UiState ui = {
        .source_root = (char *)source_root,
        .target_root = (char *)target_root,
        .tree = NULL,
        .show_target_only = false,
        .selected_index = 0,
        .scroll_offset = 0,
        .status = {0}
    };
    int rc = 0;

    setlocale(LC_ALL, "");

    if (reload_tree(&ui, NULL) != 0) {
        fprintf(stderr, "%s\n", ui.status);
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);

    while (true) {
        VisibleRow *rows = NULL;
        size_t row_count = tree_collect_visible(ui.tree->root, ui.show_target_only, &rows);
        char *selected_path = NULL;
        int ch;
        const Node *selected_node = NULL;

        if (row_count > 0 && ui.selected_index >= row_count) {
            ui.selected_index = row_count - 1;
        }
        if (row_count > 0) {
            selected_node = rows[ui.selected_index].node;
            if (selected_node->origin == ORIGIN_SOURCE) {
                selected_path = dup_string(selected_node->relative_path);
            }
        }

        rc = finish_completed_jobs(&ui, selected_path);
        free(selected_path);
        if (rc < 0) {
            free(rows);
            endwin();
            fprintf(stderr, "%s\n", ui.status);
            free_jobs(&ui);
            tree_free(ui.tree);
            return 1;
        }
        if (rc > 0) {
            free(rows);
            continue;
        }

        if (row_count == 0) {
            ui.selected_index = 0;
            ui.scroll_offset = 0;
        } else if (ui.selected_index >= row_count) {
            ui.selected_index = row_count - 1;
        }

        render(&ui, rows, row_count);
        ch = getch();

        if (ch == ERR) {
            free(rows);
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            if (ui.job_count > 0 && !confirm_quit_with_queue(ui.job_count)) {
                set_status(&ui, "Quit cancelled.");
                free(rows);
                continue;
            }
            free(rows);
            break;
        }

        if (ch == '?' ) {
            show_help();
            free(rows);
            continue;
        }

        if (ch == 'r' || ch == 'R') {
            const char *selected_path = NULL;
            if (row_count > 0 && rows[ui.selected_index].node->origin == ORIGIN_SOURCE) {
                selected_path = rows[ui.selected_index].node->relative_path;
            }
            rc = reload_tree(&ui, selected_path);
            free(rows);
            if (rc != 0) {
                endwin();
                fprintf(stderr, "%s\n", ui.status);
                free_jobs(&ui);
                tree_free(ui.tree);
                return 1;
            }
            continue;
        }

        if (ch == 'a' || ch == 'A') {
            if (ui.show_target_only && row_count > 0 && rows[ui.selected_index].node->origin == ORIGIN_TARGET_ONLY) {
                const Node *cursor = rows[ui.selected_index].node;
                int fallback_index = -1;
                while (cursor != NULL && cursor->origin != ORIGIN_SOURCE) {
                    cursor = cursor->parent;
                }
                if (cursor != NULL) {
                    fallback_index = find_row_index_for_node(rows, row_count, cursor);
                }
                if (fallback_index >= 0) {
                    ui.selected_index = (size_t)fallback_index;
                } else {
                    ui.selected_index = 0;
                }
            }
            ui.show_target_only = !ui.show_target_only;
            set_status(&ui, "Target-only items are now %s.", ui.show_target_only ? "visible" : "hidden");
            free(rows);
            continue;
        }

        if (row_count == 0) {
            set_status(&ui, "The source directory is empty.");
            free(rows);
            continue;
        }

        switch (ch) {
            case KEY_UP:
            case 'k':
                if (ui.selected_index > 0) {
                    --ui.selected_index;
                }
                break;

            case KEY_DOWN:
            case 'j':
                if (ui.selected_index + 1 < row_count) {
                    ++ui.selected_index;
                }
                break;

            case KEY_RIGHT:
            case 'l':
                if (rows[ui.selected_index].node->kind == NODE_DIR) {
                    rows[ui.selected_index].node->expanded = true;
                }
                break;

            case KEY_LEFT:
            case 'h':
                if (rows[ui.selected_index].node->kind == NODE_DIR && rows[ui.selected_index].node->expanded) {
                    rows[ui.selected_index].node->expanded = false;
                } else if (rows[ui.selected_index].node->parent != NULL &&
                           rows[ui.selected_index].node->parent->parent != NULL) {
                    int parent_index = find_row_index_for_node(rows, row_count, rows[ui.selected_index].node->parent);
                    if (parent_index >= 0) {
                        ui.selected_index = (size_t)parent_index;
                    }
                }
                break;

            case '\n':
            case KEY_ENTER:
                rc = queue_selected_node(&ui, rows[ui.selected_index].node, JOB_COPY);
                free(rows);
                (void)rc;
                continue;

            case KEY_DC:
                rc = queue_selected_node(&ui, rows[ui.selected_index].node, JOB_REMOVE);
                free(rows);
                (void)rc;
                continue;

            default:
                break;
        }

        free(rows);
    }

    endwin();
    free_jobs(&ui);
    tree_free(ui.tree);
    return 0;
}
