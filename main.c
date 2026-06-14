/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "subsync.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *dup_string(const char *value) {
    size_t len = strlen(value);
    char *copy = malloc(len + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, len + 1);
    return copy;
}

static void trim_trailing_newlines(char *value) {
    size_t len = strlen(value);

    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[len - 1] = '\0';
        --len;
    }
}

static char *expand_home(const char *input) {
    const char *home;
    size_t home_len;
    size_t input_len;
    char *expanded;

    if (input[0] != '~') {
        return dup_string(input);
    }

    home = getenv("HOME");
    if (home == NULL) {
        return dup_string(input);
    }

    if (input[1] != '\0' && input[1] != '/') {
        return dup_string(input);
    }

    home_len = strlen(home);
    input_len = strlen(input);
    expanded = malloc(home_len + input_len + 1);
    if (expanded == NULL) {
        return NULL;
    }

    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, input + 1, input_len);
    expanded[home_len + input_len - 1] = '\0';
    return expanded;
}

static char *canonicalize_directory(const char *input, char *err, size_t err_size) {
    char *expanded;
    char *resolved;
    struct stat st;

    expanded = expand_home(input);
    if (expanded == NULL) {
        snprintf(err, err_size, "Out of memory while reading '%s'", input);
        return NULL;
    }

    resolved = realpath(expanded, NULL);
    if (resolved == NULL) {
        snprintf(err, err_size, "Cannot resolve '%s': %s", expanded, strerror(errno));
        free(expanded);
        return NULL;
    }
    free(expanded);

    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_size, "'%s' is not a directory", resolved);
        free(resolved);
        return NULL;
    }

    return resolved;
}

static bool try_dialog_command(const char *command, char **out_path) {
    FILE *pipe;
    char *line = NULL;
    size_t size = 0;
    int status;

    *out_path = NULL;
    pipe = popen(command, "r");
    if (pipe == NULL) {
        return false;
    }

    if (getline(&line, &size, pipe) < 0) {
        free(line);
        pclose(pipe);
        return false;
    }

    trim_trailing_newlines(line);
    status = pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || line[0] == '\0') {
        free(line);
        return false;
    }

    *out_path = line;
    return true;
}

static bool try_graphical_picker(const char *title, char **out_path) {
    char command[256];

    if (getenv("DISPLAY") == NULL && getenv("WAYLAND_DISPLAY") == NULL) {
        return false;
    }

    snprintf(command, sizeof(command), "zenity --file-selection --directory --title=\"%s\"", title);
    if (try_dialog_command(command, out_path)) {
        return true;
    }

    snprintf(command, sizeof(command), "kdialog --getexistingdirectory . --title \"%s\"", title);
    return try_dialog_command(command, out_path);
}

static bool prompt_for_directory(const char *label, char **out_path) {
    char *line = NULL;
    size_t size = 0;

    fprintf(stderr, "%s: ", label);
    fflush(stderr);

    if (getline(&line, &size, stdin) < 0) {
        free(line);
        return false;
    }

    trim_trailing_newlines(line);
    if (line[0] == '\0') {
        free(line);
        return false;
    }

    *out_path = line;
    return true;
}

static bool obtain_directory(const char *dialog_title, const char *prompt_label, char **out_path) {
    char *raw_path = NULL;
    char err[512];

    *out_path = NULL;

    if (try_graphical_picker(dialog_title, &raw_path)) {
        char *resolved = canonicalize_directory(raw_path, err, sizeof(err));
        free(raw_path);
        if (resolved != NULL) {
            *out_path = resolved;
            return true;
        }
        fprintf(stderr, "%s\n", err);
    }

    while (prompt_for_directory(prompt_label, &raw_path)) {
        char *resolved = canonicalize_directory(raw_path, err, sizeof(err));
        free(raw_path);
        raw_path = NULL;
        if (resolved != NULL) {
            *out_path = resolved;
            return true;
        }
        fprintf(stderr, "%s\n", err);
    }

    return false;
}

static bool path_is_same_or_nested(const char *left, const char *right) {
    size_t left_len = strlen(left);

    if (strncmp(left, right, left_len) != 0) {
        return false;
    }

    if (right[left_len] == '\0') {
        return true;
    }

    if (strcmp(left, "/") == 0) {
        return true;
    }

    return right[left_len] == '/';
}

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [SOURCE_DIR TARGET_DIR]\n", argv0);
}

int main(int argc, char **argv) {
    char *source_root = NULL;
    char *target_root = NULL;
    char err[512];
    int rc;

    if (argc != 1 && argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 3) {
        source_root = canonicalize_directory(argv[1], err, sizeof(err));
        if (source_root == NULL) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }

        target_root = canonicalize_directory(argv[2], err, sizeof(err));
        if (target_root == NULL) {
            fprintf(stderr, "%s\n", err);
            free(source_root);
            return 1;
        }
    } else {
        if (!obtain_directory("Select Source Directory", "Source directory", &source_root)) {
            return 1;
        }

        if (!obtain_directory("Select Target Directory", "Target directory", &target_root)) {
            free(source_root);
            return 1;
        }
    }

    if (path_is_same_or_nested(source_root, target_root) ||
        path_is_same_or_nested(target_root, source_root)) {
        fprintf(stderr, "Source and target directories must not contain each other.\n");
        free(source_root);
        free(target_root);
        return 1;
    }

    rc = ui_run(source_root, target_root);
    free(source_root);
    free(target_root);
    return rc;
}
