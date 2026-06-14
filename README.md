<!-- SPDX-FileCopyrightText: 2026 Lambdara -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# subsync

`subsync` is a terminal file sync browser written in vanilla C with `ncurses`.
It shows the source directory as a tree, compares it against a target directory,
and lets you copy or remove items on the target by toggling checkboxes in the UI.

The intended use case is a local source directory and a phone directory that is
mounted and accessible from the PC as a normal filesystem path, for example, to 
add and remove parts of your music library.

## What it does

- Compares a source directory to a target directory by relative path and item type.
- Shows the source side by default.
- Marks files as checked only when the same relative file exists on the target.
- Marks directories as checked only when the full source subtree exists on the target.
- Lets you copy missing files and directories from source to target.
- Lets you remove matching files and directories from the target.
- Hides target-only items by default, with an option to reveal them for inspection.
- Warns before an action would delete target content that does not exist in the source.

## Requirements

- A C compiler with C17 support.
- `ncursesw` development files.
- `pkg-config` for the `Makefile` build path.
- `cmake` if you want to use the CMake build path.
- Optional: `zenity` or `kdialog` for graphical directory selection.

On Linux, the target directory can be any mounted path that behaves like a normal
directory. If your phone is exposed through GVFS, FUSE, MTP, or a desktop file
manager mount, behavior depends on how well that mount supports normal POSIX file
operations.

## Building

Using `make`:

```bash
make
```

Using CMake:

```bash
cmake -S . -B build
cmake --build build
```

## Running

Pass both directories explicitly:

```bash
./subsync /path/to/source /path/to/target
```

Or start without arguments:

```bash
./subsync
```

Without arguments, `subsync` tries to open a graphical directory picker first.
If that is unavailable or fails, it falls back to prompting for paths in the terminal.

Source and target must be different directories, and neither may contain the other.

## Controls

- `?` show the controls popup
- `q` quit
- `Enter` or `Space` toggle the selected source item
- `Up` / `Down` move through the visible list
- `Right` expand the selected directory
- `Left` collapse the selected directory, or move to its parent
- `a` show or hide target-only items
- `r` reload the directory trees from disk
- `j` / `k` move down and up
- `h` / `l` collapse and expand

## UI markers

- `[x]` item exists on the target
- `[ ]` item is missing on the target
- `~` source subtree is only partially present on the target
- `!` toggling this node will delete or replace extra target content
- `[target]` target-only item shown when extras are visible; it cannot be toggled

## Sync semantics

- Checking a file copies that file to the target at the same relative path.
- Checking a directory creates that directory on the target and recursively copies
  the full source subtree below it.
- Unchecking a file removes that file from the target.
- Unchecking a directory removes that directory and everything below it from the target.
- If a target path exists with the wrong type, `subsync` can replace it after warning.
- If a checked directory contains extra target-only items, removing it triggers a warning.

## Current limitations

- Overlap is based on existence and type, not file contents, timestamps, or hashes.
- Only regular files and directories on the source side are included.
- Unsupported source entries are skipped.
- Target-only items are non-interactive even when shown.

## License

This project is licensed under GPL-3.0-only. See [LICENSE](LICENSE).
