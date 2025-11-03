# SimpleSync

SimpleSync is a single-process directory mirror inspired by the `dsync` tool in
MPIFileUtils. It keeps a destination tree in sync with a source tree while
emitting detailed progress, performance metrics, and per-entry metadata derived
from `lstat(2)`.

## Features
- Validates source and destination paths with clear status updates.
- Recursively copies new or modified files, creating intermediate directories as needed.
- Optionally prunes files that no longer exist in the source (enabled by default).
- Skips symbolic links and non-regular files with informative warnings.
- Measures elapsed time per stage and overall throughput.
- Records and prints `FileMetadata` (path, depth, mode, uid/gid, timestamps, size) for every synchronized source entry.

## Build

Requirements:
- C++17 compiler (tested with `g++` 9+ / `clang++` 11+)
- POSIX-compatible platform (macOS or Linux)

From `metadata_for_sync`:

```bash
g++ -std=c++17 -O2 -Iinclude src/main.cpp src/sync.cpp -o simplesync
```

## Usage

```bash
./simplesync [--keep-extra] <source_dir> <destination_dir>
```

- `source_dir`: directory to mirror.
- `destination_dir`: directory to update.
- `--keep-extra`: preserve entries that exist only in the destination (skip prune stage).

The program logs each phase (validation, copy, optional prune), prints a summary
of counts and throughput, and finishes with a metadata dump for synchronized
entries.

## Tests

Fixture trees live under `testdata/`. A lightweight regression harness in
`tests/test_sync.cpp` runs both pruning modes and verifies synchronized content.

Build and execute:

```bash
g++ -std=c++17 -O2 -Iinclude tests/test_sync.cpp src/sync.cpp -o sync_tests
./sync_tests
```

The test binary copies the fixtures into temporary directories to keep the
samples intact and reports metadata for each run. A successful execution prints
`All tests passed.` at the end.
