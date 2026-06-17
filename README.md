<!-- SPDX-FileCopyrightText: 2026 Lambdara -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# subsync

`subsync` is a terminal file sync browser written in vanilla C with `ncurses`.
It shows the source directory as a tree, compares it against a target directory,
and lets you queue copy or remove operations on the target from the UI.

The intended use case is a local source directory and a phone directory that is
mounted and accessible from the PC as a normal filesystem path, for example, to
add and remove parts of your music library.

## What it does

- Compares a source directory to a target directory by relative path and item type.
- Shows the source side by default.
- Marks files as checked only when the same relative file exists on the target.
- Marks directories as checked only when the full source subtree exists on the target.
- Lets you queue missing files and directories for copy from source to target.
- Lets you queue matching files and directories for removal from the target.
- Keeps the UI responsive while transfers are running.
- Runs one transfer at a time and keeps the rest in an internal queue.
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

`subsync` intentionally runs one transfer at a time. The UI stays responsive, but
target-side operations are serialized because queued transfers are safer and more
predictable, especially on GVFS/MTP-backed phone storage.

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
- `Enter` check the selected source item and queue a copy
- `Delete` uncheck the selected source item and queue removal
- `Up` / `Down` move through the visible list
- `Right` expand the selected directory
- `Left` collapse the selected directory, or move to its parent
- `a` show or hide target-only items
- `r` reload the directory trees from disk
- `j` / `k` move down and up
- `h` / `l` collapse and expand

The top-right queue indicator shows the current total queued workload in files and MB.

## UI markers

- `[x]` item exists on the target
- `[ ]` item is missing on the target
- `d` target directory exists, but none of its source contents exist below it
- `~` source subtree is only partially present on the target
- `!` checking or deleting this node will delete or replace extra target content
- `*` a transfer is queued or running for this item or an overlapping subtree
- `[target]` target-only item shown when extras are visible; it cannot be toggled

## Sync semantics

- Pressing `Enter` on an unchecked file queues that file for copy to the target at
  the same relative path.
- Pressing `Enter` on an unchecked directory queues creation of that directory on
  the target and recursive copy of the full source subtree below it.
- Pressing `Delete` on a checked file queues removal of that file from the target.
- Pressing `Delete` on a checked directory queues removal of that directory and
  everything below it from the target.
- If a target path exists with the wrong type, `subsync` can replace it after warning.
- If a checked directory contains extra target-only items, deleting it triggers a warning.
- Overlapping operations are blocked while a transfer is already queued or running
  for the same path, a parent path, or a child path.
- Queued transfers start automatically when the current transfer finishes.
- Quitting while transfers are still active asks for confirmation. The current
  transfer continues in the background if you confirm the quit, but the remaining
  internal queue is discarded.

## Current limitations

- Overlap is based on existence and type, not file contents, timestamps, or hashes.
- Only regular files and directories on the source side are included.
- Unsupported source entries are skipped.
- Target-only items are non-interactive even when shown.

## License

This project is licensed under GPL-3.0-only. See [LICENSE](LICENSE).
