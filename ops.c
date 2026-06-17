/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "subsync.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    TransferProgressFn fn;
    void *userdata;
    size_t files_completed;
    unsigned long long bytes_completed;
} TransferContext;

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

static void report_transfer_progress(
    TransferContext *ctx,
    const char *current_path,
    unsigned long long current_file_bytes,
    unsigned long long current_file_total
) {
    TransferProgress progress;

    if (ctx == NULL || ctx->fn == NULL) {
        return;
    }

    progress.current_path = current_path;
    progress.files_completed = ctx->files_completed;
    progress.bytes_completed = ctx->bytes_completed;
    progress.current_file_bytes = current_file_bytes;
    progress.current_file_total = current_file_total;
    ctx->fn(&progress, ctx->userdata);
}

static void finish_transferred_file(TransferContext *ctx, unsigned long long file_size) {
    if (ctx == NULL) {
        return;
    }

    ++ctx->files_completed;
    ctx->bytes_completed += file_size;
    report_transfer_progress(ctx, NULL, 0, 0);
}

static bool env_flag_enabled(const char *name) {
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

bool subsync_target_uses_gio(const char *path) {
    if (env_flag_enabled("SUBSYNC_FORCE_GIO")) {
        return true;
    }

    return path != NULL &&
           strncmp(path, "/run/user/", 10) == 0 &&
           strstr(path, "/gvfs/") != NULL;
}

bool subsync_target_uses_android_mtp(const char *path) {
    const char *gvfs;

    if (env_flag_enabled("SUBSYNC_FORCE_ANDROID_MTP") ||
        env_flag_enabled("SUBSYNC_FORCE_MTP")) {
        return true;
    }

    if (path == NULL) {
        return false;
    }

    gvfs = strstr(path, "/gvfs/");
    if (gvfs == NULL) {
        return false;
    }

    gvfs += strlen("/gvfs/");
    return strncmp(gvfs, "mtp:", 4) == 0;
}

static void trim_trailing_newlines(char *value) {
    size_t len;

    if (value == NULL) {
        return;
    }

    len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[len - 1] = '\0';
        --len;
    }
}

static bool android_mtp_filename_char_is_forbidden(unsigned char ch) {
    if (ch <= 0x1f || ch == 0x7f) {
        return true;
    }

    switch (ch) {
        case '"':
        case '*':
        case '/':
        case ':':
        case '<':
        case '>':
        case '?':
        case '\\':
        case '|':
            return true;
        default:
            return false;
    }
}

static void format_android_mtp_forbidden_char(unsigned char ch, char *buffer, size_t buffer_size) {
    switch (ch) {
        case '"':
            snprintf(buffer, buffer_size, "'\"'");
            return;
        case '\\':
            snprintf(buffer, buffer_size, "'\\\\'");
            return;
        case 0x7f:
            snprintf(buffer, buffer_size, "control character 0x7F");
            return;
        default:
            if (ch < 0x20) {
                snprintf(buffer, buffer_size, "control character 0x%02X", ch);
            } else {
                snprintf(buffer, buffer_size, "'%c'", ch);
            }
            return;
    }
}

static int describe_android_mtp_filename_problem(
    const char *name,
    char *problem,
    size_t problem_size
) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
        snprintf(problem, problem_size, "is empty");
        return -1;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        snprintf(problem, problem_size, "is reserved");
        return -1;
    }

    for (i = 0; name[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)name[i];

        if (android_mtp_filename_char_is_forbidden(ch)) {
            char char_text[64];

            format_android_mtp_forbidden_char(ch, char_text, sizeof(char_text));
            snprintf(problem, problem_size, "contains %s", char_text);
            return -1;
        }
    }

    return 0;
}

static int validate_android_mtp_filename_component(
    const char *target_root,
    const char *relative_path,
    const char *display_name,
    char *err,
    size_t err_size
) {
    char problem[96];

    if (!subsync_target_uses_android_mtp(target_root)) {
        return 0;
    }

    if (describe_android_mtp_filename_problem(display_name, problem, sizeof(problem)) != 0) {
        set_error(
            err,
            err_size,
            "Android/MTP target cannot accept '%s': filename component '%s' %s. "
            "Rename it before copying to the phone. Forbidden characters: \" * / : < > ? \\ | and control characters.",
            relative_path != NULL && relative_path[0] != '\0' ? relative_path : display_name,
            display_name != NULL ? display_name : "",
            problem
        );
        return -1;
    }

    return 0;
}

static int validate_android_mtp_source_node_recursive(
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size
) {
    size_t i;

    if (node->origin != ORIGIN_SOURCE) {
        return 0;
    }

    if (validate_android_mtp_filename_component(target_root, node->relative_path, node->name, err, err_size) != 0) {
        return -1;
    }

    for (i = 0; i < node->child_count; ++i) {
        if (validate_android_mtp_source_node_recursive(target_root, node->children[i], err, err_size) != 0) {
            return -1;
        }
    }

    return 0;
}

int subsync_validate_source_node_for_target(
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

    return validate_android_mtp_source_node_recursive(target_root, node, err, err_size);
}

static int run_command_capture_stderr(const char *const argv[], char *err, size_t err_size) {
    int pipe_fds[2];
    int devnull_fd = -1;
    pid_t pid;
    int status;
    char *stderr_buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;
    char chunk[256];
    ssize_t bytes_read;

    if (pipe(pipe_fds) != 0) {
        set_error(err, err_size, "Cannot create subprocess pipe: %s", strerror(errno));
        return -1;
    }

    devnull_fd = open("/dev/null", O_RDONLY);
    if (devnull_fd < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_error(err, err_size, "Cannot open /dev/null: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(devnull_fd);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_error(err, err_size, "Cannot fork subprocess: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        close(pipe_fds[0]);

        if (dup2(devnull_fd, STDIN_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }

        close(devnull_fd);
        close(pipe_fds[1]);
        execvp(argv[0], (char *const *)argv);
        dprintf(STDERR_FILENO, "Cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(devnull_fd);
    close(pipe_fds[1]);

    while ((bytes_read = read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
        if (used + (size_t)bytes_read + 1 > capacity) {
            size_t next_capacity = capacity == 0 ? 512 : capacity;
            char *grown;

            while (used + (size_t)bytes_read + 1 > next_capacity) {
                next_capacity *= 2;
            }

            grown = realloc(stderr_buffer, next_capacity);
            if (grown == NULL) {
                free(stderr_buffer);
                close(pipe_fds[0]);
                waitpid(pid, NULL, 0);
                set_error(err, err_size, "Out of memory while reading subprocess output");
                return -1;
            }

            stderr_buffer = grown;
            capacity = next_capacity;
        }

        memcpy(stderr_buffer + used, chunk, (size_t)bytes_read);
        used += (size_t)bytes_read;
    }
    close(pipe_fds[0]);

    if (bytes_read < 0) {
        free(stderr_buffer);
        set_error(err, err_size, "Cannot read subprocess output: %s", strerror(errno));
        waitpid(pid, NULL, 0);
        return -1;
    }

    if (stderr_buffer == NULL) {
        stderr_buffer = calloc(1, 1);
        if (stderr_buffer == NULL) {
            waitpid(pid, NULL, 0);
            set_error(err, err_size, "Out of memory while reading subprocess output");
            return -1;
        }
    }
    stderr_buffer[used] = '\0';
    trim_trailing_newlines(stderr_buffer);

    if (waitpid(pid, &status, 0) < 0) {
        free(stderr_buffer);
        set_error(err, err_size, "Cannot wait for subprocess: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        free(stderr_buffer);
        return 0;
    }

    if (stderr_buffer[0] != '\0') {
        set_error(err, err_size, "%s", stderr_buffer);
    } else if (WIFEXITED(status)) {
        set_error(err, err_size, "%s exited with status %d", argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        set_error(err, err_size, "%s terminated by signal %d", argv[0], WTERMSIG(status));
    } else {
        set_error(err, err_size, "%s failed", argv[0]);
    }

    free(stderr_buffer);
    return -1;
}

static int gio_make_directory_with_parents(const char *path, char *err, size_t err_size) {
    const char *argv[] = {"gio", "mkdir", "-p", path, NULL};

    return run_command_capture_stderr(argv, err, err_size);
}

static int gio_remove_path(const char *path, char *err, size_t err_size) {
    const char *argv[] = {"gio", "remove", "-f", path, NULL};

    return run_command_capture_stderr(argv, err, err_size);
}

static bool is_unit_letter(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static int parse_size_token(const char *text, unsigned long long *out_bytes, const char **out_rest) {
    char *end = NULL;
    double value;
    char unit[16];
    size_t unit_len = 0;
    double factor = 1.0;

    value = strtod(text, &end);
    if (end == text) {
        return -1;
    }

    while (*end != '\0' && !is_unit_letter((unsigned char)*end)) {
        ++end;
    }

    while (is_unit_letter((unsigned char)*end) && unit_len + 1 < sizeof(unit)) {
        unit[unit_len++] = *end;
        ++end;
    }
    unit[unit_len] = '\0';

    if (unit_len == 0) {
        return -1;
    }

    if (strcmp(unit, "bytes") == 0 || strcmp(unit, "B") == 0) {
        factor = 1.0;
    } else if (strcmp(unit, "kB") == 0 || strcmp(unit, "KB") == 0) {
        factor = 1000.0;
    } else if (strcmp(unit, "MB") == 0) {
        factor = 1000.0 * 1000.0;
    } else if (strcmp(unit, "GB") == 0) {
        factor = 1000.0 * 1000.0 * 1000.0;
    } else if (strcmp(unit, "TB") == 0) {
        factor = 1000.0 * 1000.0 * 1000.0 * 1000.0;
    } else if (strcmp(unit, "KiB") == 0) {
        factor = 1024.0;
    } else if (strcmp(unit, "MiB") == 0) {
        factor = 1024.0 * 1024.0;
    } else if (strcmp(unit, "GiB") == 0) {
        factor = 1024.0 * 1024.0 * 1024.0;
    } else if (strcmp(unit, "TiB") == 0) {
        factor = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    } else {
        return -1;
    }

    *out_bytes = (unsigned long long)(value * factor + 0.5);
    if (out_rest != NULL) {
        *out_rest = end;
    }
    return 0;
}

static void strip_control_sequences(const char *input, char *output, size_t output_size) {
    size_t used = 0;
    const unsigned char *cursor = (const unsigned char *)input;

    if (output_size == 0) {
        return;
    }

    while (*cursor != '\0' && used + 1 < output_size) {
        if (*cursor == '\033' && cursor[1] == '[') {
            cursor += 2;
            while (*cursor != '\0' && !((*cursor >= '@' && *cursor <= '~'))) {
                ++cursor;
            }
            if (*cursor != '\0') {
                ++cursor;
            }
            continue;
        }

        if (*cursor >= 0x20 || *cursor == '\t') {
            output[used++] = (char)*cursor;
        }
        ++cursor;
    }

    output[used] = '\0';
    trim_trailing_newlines(output);
}

static int parse_gio_progress_line(const char *line, unsigned long long *out_done, unsigned long long *out_total) {
    const char *marker;
    const char *cursor;

    marker = strstr(line, "Transferred ");
    if (marker == NULL) {
        return -1;
    }

    cursor = marker + strlen("Transferred ");
    if (parse_size_token(cursor, out_done, &cursor) != 0) {
        return -1;
    }

    marker = strstr(cursor, "out of");
    if (marker == NULL) {
        return -1;
    }

    cursor = marker + strlen("out of");
    if (parse_size_token(cursor, out_total, &cursor) != 0) {
        return -1;
    }

    return 0;
}

static int gio_copy_file(
    const char *source_path,
    const char *target_path,
    const char *relative_path,
    TransferContext *progress,
    char *err,
    size_t err_size
) {
    const char *argv[] = {"gio", "copy", "-p", "-T", "--default-permissions", source_path, target_path, NULL};
    int pipe_fds[2];
    int devnull_fd = -1;
    pid_t pid;
    int status;
    char line_buffer[512];
    size_t line_used = 0;
    char last_output[512] = "";
    char chunk[256];
    ssize_t bytes_read;

    if (pipe(pipe_fds) != 0) {
        set_error(err, err_size, "Cannot create subprocess pipe: %s", strerror(errno));
        return -1;
    }

    devnull_fd = open("/dev/null", O_RDONLY);
    if (devnull_fd < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_error(err, err_size, "Cannot open /dev/null: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(devnull_fd);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        set_error(err, err_size, "Cannot fork subprocess: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        setenv("LC_ALL", "C", 1);
        close(pipe_fds[0]);

        if (dup2(devnull_fd, STDIN_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            dprintf(pipe_fds[1], "dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }

        close(devnull_fd);
        close(pipe_fds[1]);
        execvp(argv[0], (char *const *)argv);
        dprintf(STDERR_FILENO, "Cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(devnull_fd);
    close(pipe_fds[1]);

    while ((bytes_read = read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
        size_t i;

        for (i = 0; i < (size_t)bytes_read; ++i) {
            unsigned char ch = (unsigned char)chunk[i];

            if (ch == '\r' || ch == '\n') {
                char cleaned[512];
                unsigned long long transferred = 0;
                unsigned long long total = 0;

                line_buffer[line_used] = '\0';
                strip_control_sequences(line_buffer, cleaned, sizeof(cleaned));
                if (cleaned[0] != '\0') {
                    if (parse_gio_progress_line(cleaned, &transferred, &total) == 0) {
                        report_transfer_progress(progress, relative_path, transferred, total);
                    } else {
                        snprintf(last_output, sizeof(last_output), "%s", cleaned);
                    }
                }
                line_used = 0;
                continue;
            }

            if (line_used + 1 < sizeof(line_buffer)) {
                line_buffer[line_used++] = (char)ch;
            }
        }
    }
    close(pipe_fds[0]);

    if (bytes_read < 0) {
        set_error(err, err_size, "Cannot read subprocess output: %s", strerror(errno));
        waitpid(pid, NULL, 0);
        return -1;
    }

    if (line_used > 0) {
        char cleaned[512];
        unsigned long long transferred = 0;
        unsigned long long total = 0;

        line_buffer[line_used] = '\0';
        strip_control_sequences(line_buffer, cleaned, sizeof(cleaned));
        if (cleaned[0] != '\0') {
            if (parse_gio_progress_line(cleaned, &transferred, &total) == 0) {
                report_transfer_progress(progress, relative_path, transferred, total);
            } else {
                snprintf(last_output, sizeof(last_output), "%s", cleaned);
            }
        }
    }

    if (waitpid(pid, &status, 0) < 0) {
        set_error(err, err_size, "Cannot wait for subprocess: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    if (last_output[0] != '\0') {
        set_error(err, err_size, "%s", last_output);
    } else if (WIFEXITED(status)) {
        set_error(err, err_size, "%s exited with status %d", argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        set_error(err, err_size, "%s terminated by signal %d", argv[0], WTERMSIG(status));
    } else {
        set_error(err, err_size, "%s failed", argv[0]);
    }

    return -1;
}

static const char *path_basename(const char *path) {
    const char *slash;

    if (path == NULL) {
        return "";
    }

    slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static void rewrite_gvfs_error_if_needed(
    const char *source_path,
    const char *target_path,
    char *err,
    size_t err_size
) {
    const char *source_name;

    if (err == NULL || err[0] == '\0') {
        return;
    }

    if (!subsync_target_uses_gio(target_path)) {
        return;
    }

    source_name = path_basename(source_path);

    if (strstr(err, "libmtp error:  Could not send object.") != NULL &&
        validate_android_mtp_filename_component(target_path, source_name, source_name, err, err_size) != 0) {
        return;
    }

    if (strstr(err, "libmtp error") != NULL) {
        char original[512];

        snprintf(original, sizeof(original), "%s", err);
        set_error(
            err,
            err_size,
            "MTP transfer failed for '%s': %s",
            source_name,
            original
        );
    }
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

    if (subsync_target_uses_gio(path)) {
        return gio_make_directory_with_parents(path, err, err_size);
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
        if (errno == EOPNOTSUPP || errno == ENOSYS) {
            return gio_make_directory_with_parents(path, err, err_size);
        }
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
        if (subsync_target_uses_gio(path)) {
            if (gio_remove_path(path, err, err_size) != 0) {
                return -1;
            }
        } else if (unlink(path) != 0) {
            if (errno == EOPNOTSUPP || errno == ENOSYS) {
                return gio_remove_path(path, err, err_size);
            }
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

    if (subsync_target_uses_gio(path)) {
        if (gio_remove_path(path, err, err_size) != 0) {
            return -1;
        }
    } else if (rmdir(path) != 0) {
        if (errno == EOPNOTSUPP || errno == ENOSYS) {
            return gio_remove_path(path, err, err_size);
        }
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

static int copy_file_contents(
    const char *source_path,
    const char *target_path,
    const char *relative_path,
    TransferContext *progress,
    char *err,
    size_t err_size
) {
    int source_fd = -1;
    int target_fd = -1;
    struct stat st;
    char buffer[65536];
    ssize_t bytes_read;
    unsigned long long current_file_bytes = 0;

    if (ensure_parent_directory(target_path, err, err_size) != 0) {
        return -1;
    }

    if (stat(source_path, &st) != 0) {
        set_error(err, err_size, "Cannot inspect '%s': %s", source_path, strerror(errno));
        return -1;
    }

    if (subsync_target_uses_gio(source_path) || subsync_target_uses_gio(target_path)) {
        int rc;

        report_transfer_progress(progress, relative_path, 0, (unsigned long long)st.st_size);
        rc = gio_copy_file(source_path, target_path, relative_path, progress, err, err_size);
        if (rc != 0) {
            rewrite_gvfs_error_if_needed(source_path, target_path, err, err_size);
            return rc;
        }

        report_transfer_progress(progress, relative_path, (unsigned long long)st.st_size, (unsigned long long)st.st_size);
        finish_transferred_file(progress, (unsigned long long)st.st_size);
        return 0;
    }

    source_fd = open(source_path, O_RDONLY);
    if (source_fd < 0) {
        set_error(err, err_size, "Cannot open '%s': %s", source_path, strerror(errno));
        return -1;
    }

    target_fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (target_fd < 0) {
        if (errno == EOPNOTSUPP || errno == ENOSYS) {
            int rc;

            close(source_fd);
            report_transfer_progress(progress, relative_path, 0, (unsigned long long)st.st_size);
            rc = gio_copy_file(source_path, target_path, relative_path, progress, err, err_size);
            if (rc != 0) {
                rewrite_gvfs_error_if_needed(source_path, target_path, err, err_size);
                return rc;
            }
            report_transfer_progress(progress, relative_path, (unsigned long long)st.st_size, (unsigned long long)st.st_size);
            finish_transferred_file(progress, (unsigned long long)st.st_size);
            return 0;
        }
        set_error(err, err_size, "Cannot create '%s': %s", target_path, strerror(errno));
        close(source_fd);
        return -1;
    }

    report_transfer_progress(progress, relative_path, 0, (unsigned long long)st.st_size);

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

        current_file_bytes += (unsigned long long)bytes_read;
        report_transfer_progress(progress, relative_path, current_file_bytes, (unsigned long long)st.st_size);
    }

    if (bytes_read < 0) {
        set_error(err, err_size, "Cannot read '%s': %s", source_path, strerror(errno));
        close(source_fd);
        close(target_fd);
        return -1;
    }

    close(source_fd);
    close(target_fd);
    report_transfer_progress(progress, relative_path, (unsigned long long)st.st_size, (unsigned long long)st.st_size);
    finish_transferred_file(progress, (unsigned long long)st.st_size);
    return 0;
}

static int copy_source_node_recursive(
    const char *source_root,
    const char *target_root,
    const Node *node,
    TransferContext *progress,
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

    if (validate_android_mtp_filename_component(target_root, node->relative_path, node->name, err, err_size) != 0) {
        free(source_path);
        free(target_path);
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

        if (copy_file_contents(source_path, target_path, node->relative_path, progress, err, err_size) != 0) {
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
        if (copy_source_node_recursive(source_root, target_root, node->children[i], progress, err, err_size) != 0) {
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
    size_t err_size,
    TransferProgressFn progress_fn,
    void *progress_userdata
) {
    TransferContext progress = {
        .fn = progress_fn,
        .userdata = progress_userdata,
        .files_completed = 0,
        .bytes_completed = 0
    };

    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }

    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        set_error(err, err_size, "No source item is selected");
        return -1;
    }

    if (subsync_validate_source_node_for_target(target_root, node, err, err_size) != 0) {
        return -1;
    }

    return copy_source_node_recursive(source_root, target_root, node, &progress, err, err_size);
}

int remove_target_for_source_node(
    const char *target_root,
    const Node *node,
    char *err,
    size_t err_size,
    TransferProgressFn progress_fn,
    void *progress_userdata
) {
    char *target_path;
    int rc;

    (void)progress_fn;
    (void)progress_userdata;

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
