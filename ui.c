/* SPDX-FileCopyrightText: 2026 Lambdara */
/* SPDX-License-Identifier: GPL-3.0-only */

#include "subsync.h"

#include <locale.h>
#include <ncurses.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *source_root;
    char *target_root;
    Tree *tree;
    bool show_target_only;
    size_t selected_index;
    size_t scroll_offset;
    char status[512];
} UiState;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

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

static void format_row(char *buffer, size_t buffer_size, const VisibleRow *row) {
    const Node *node = row->node;
    int indent = row->depth * 2;
    char expand = node->kind == NODE_DIR ? (node->expanded ? 'v' : '>') : ' ';
    const char *suffix = node->kind == NODE_DIR ? "/" : "";

    if (node->origin == ORIGIN_TARGET_ONLY) {
        snprintf(
            buffer,
            buffer_size,
            "%*s%c [target] %s%s",
            indent,
            "",
            expand,
            node->name,
            suffix
        );
        return;
    }

    snprintf(
        buffer,
        buffer_size,
        "%*s%c [%c]%c %s%s",
        indent,
        "",
        expand,
        node->checked ? 'x' : ' ',
        status_marker(node),
        node->name,
        suffix
    );
}

static void render_selected_summary(const Node *node) {
    if (node == NULL) {
        mvaddnstr(LINES - 1, 0, "No items in the source directory.", COLS - 1);
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
                "%s is fully present on the target. Enter removes it and %zu extra target item(s).",
                node->relative_path[0] != '\0' ? node->relative_path : ".",
                node->hidden_target_only_count
            );
        } else {
            mvprintw(
                LINES - 1,
                0,
                "%s is present on the target. Enter removes it from the target.",
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
            "%s is not fully present. Enter will copy it and delete %zu conflicting target item(s).",
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
            "%s is partially present on the target. Enter copies the missing source items.",
            node->relative_path[0] != '\0' ? node->relative_path : "."
        );
        clrtoeol();
        return;
    }

    mvprintw(
        LINES - 1,
        0,
        "%s is missing on the target. Enter copies it to the matching target path.",
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
    ch = getch();
    return ch == 'y' || ch == 'Y';
}

static void show_help(void) {
    const char *lines[] = {
        "Up/Down  Move through the visible list",
        "Right    Expand the selected directory",
        "Left     Collapse it, or move to the parent",
        "Enter    Copy missing source items, or remove synced target items",
        "a        Show or hide target-only items",
        "r        Reload the source and target directories",
        "q        Quit",
        "",
        "[x]      Present on target; directories mean the full source subtree exists",
        "[ ]      Missing on target",
        "~        Some of the source subtree is already present on the target",
        "!        Toggling this node deletes or replaces target-only target content",
        "[target] Visible only when extras are shown; not interactable",
        "",
        "Press any key to return."
    };

    draw_popup("Controls", lines, sizeof(lines) / sizeof(lines[0]), true);
}

static void render(UiState *ui, VisibleRow *rows, size_t row_count) {
    size_t i;
    int list_top = 3;
    int list_height = LINES - 4;

    erase();
    mvprintw(0, 0, "Source: %s", ui->source_root);
    clrtoeol();
    mvprintw(1, 0, "Target: %s", ui->target_root);
    clrtoeol();
    mvprintw(2, 0, "[?] help  [a] target-only: %s  [r] reload  [q] quit", ui->show_target_only ? "shown" : "hidden");
    clrtoeol();

    clamp_scroll(ui, row_count, list_height);

    for (i = 0; i < (size_t)list_height && ui->scroll_offset + i < row_count; ++i) {
        size_t row_index = ui->scroll_offset + i;
        char line[1024];

        format_row(line, sizeof(line), &rows[row_index]);

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
        mvaddnstr(LINES - 2, 0, ui->status, COLS - 1);
        clrtoeol();
    } else {
        move(LINES - 2, 0);
        clrtoeol();
    }

    render_selected_summary(row_count > 0 ? rows[ui->selected_index].node : NULL);
    refresh();
}

static int toggle_selected_node(UiState *ui, const Node *node) {
    char err[512];
    char *selected_path = NULL;
    int rc;

    if (node == NULL || node->origin != ORIGIN_SOURCE) {
        set_status(ui, "Target-only items cannot be changed here.");
        return 0;
    }

    selected_path = dup_string(node->relative_path);
    if (selected_path == NULL) {
        set_status(ui, "Out of memory while preparing the operation.");
        return 0;
    }

    if (node->checked) {
        if (node->kind == NODE_DIR && node->hidden_target_only_count > 0) {
            if (!confirm_destructive_action("Unchecking", node->hidden_target_only_count)) {
                set_status(ui, "Operation cancelled.");
                free(selected_path);
                return 0;
            }
        }

        err[0] = '\0';
        rc = remove_target_for_source_node(ui->target_root, node, err, sizeof(err));
        if (rc != 0) {
            set_status(ui, "%s", err[0] != '\0' ? err : "Failed to remove the target item.");
            free(selected_path);
            return 0;
        }

        set_status(ui, "Removed '%s' from the target.", node->relative_path[0] != '\0' ? node->relative_path : ".");
    } else {
        if (node->replacement_delete_count > 0) {
            if (!confirm_destructive_action("Checking", node->replacement_delete_count)) {
                set_status(ui, "Operation cancelled.");
                free(selected_path);
                return 0;
            }
        }

        err[0] = '\0';
        rc = copy_source_node_to_target(ui->source_root, ui->target_root, node, err, sizeof(err));
        if (rc != 0) {
            set_status(ui, "%s", err[0] != '\0' ? err : "Failed to copy the source item.");
            free(selected_path);
            return 0;
        }

        set_status(ui, "Copied '%s' to the target.", node->relative_path[0] != '\0' ? node->relative_path : ".");
    }

    rc = reload_tree(ui, selected_path);
    free(selected_path);
    return rc;
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

    while (true) {
        VisibleRow *rows = NULL;
        size_t row_count = tree_collect_visible(ui.tree->root, ui.show_target_only, &rows);
        int ch;

        if (row_count == 0) {
            ui.selected_index = 0;
            ui.scroll_offset = 0;
        } else if (ui.selected_index >= row_count) {
            ui.selected_index = row_count - 1;
        }

        render(&ui, rows, row_count);
        ch = getch();

        if (ch == 'q' || ch == 'Q') {
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
            case ' ':
                rc = toggle_selected_node(&ui, rows[ui.selected_index].node);
                free(rows);
                if (rc != 0) {
                    endwin();
                    fprintf(stderr, "%s\n", ui.status);
                    tree_free(ui.tree);
                    return 1;
                }
                continue;

            default:
                break;
        }

        free(rows);
    }

    endwin();
    tree_free(ui.tree);
    return 0;
}
