# latch

The shared door for our gtkmm apps that move a user's files (Rouster, Tidy).
Split out of Rouster on 2026-11-10 so both apps go through ONE door.

| Header | What |
|---|---|
| `latch/Door.hpp` | the one door: a previewed plan -> journal (fsync'd intent / outcome) -> rename; undo by identity (dev + inode); the manifest replayed after a crash; refusals (outside home, Protected, symlinked parents, the app's own records, packages split). 10 verbs: trash, move, write, clone, dedupe, mkdir, copy, settings, root, **tag** (0.2: set / clear one `user.tidy.*` xattr, old value journaled, undo puts it back) |
| `latch/Protect.hpp` | the never-touch list, `fixes/`, places another of our apps owns (Samson), `*.samson` |
| `latch/Package.hpp` | packages are one thing: `.scriv` and our apps' bundles, never split |
| `latch/Vcs.hpp` | git / any program as a child process; `feed` through pipes |

At startup an app calls `latch::door::set_app_name("Tidy")`. An app with a
root helper (Rouster) also calls `set_root_rules(...)`; without it every Root
action is refused.

## Selftest

`./build.sh` also builds `build/latch_selftest` (only when latch is the top-level
project): the Tag verb on a fixture home under /tmp -- names, refusals, set /
overwrite / untag / undo, changed-since and atomic-save refusals, non-UTF-8
values, crash replay. Needs a filesystem with user xattrs (ext4, btrfs, tmpfs).

## Install

latch is a **static** library: each app compiles the door into itself (no
`.so` at run time; each app's selftest checks the exact door it ships).

```
./install.sh                  # build + install to /usr/local (sudo if needed)
./install.sh --prefix ~/.local
./uninstall.sh                # removes exactly what install.sh listed
```

Installs `include/latch/*.hpp`, `lib64/liblatch.a` (`lib/` on some distros),
`lib64/cmake/latch/` (the CMake package) and
`share/latch/install-manifest.txt`. **After changing latch: `./install.sh`
again, then rebuild the apps.**

## Use from an app

```cmake
find_package(latch 0.2 REQUIRED)
target_link_libraries(<app>_core PUBLIC latch::latch)
```

Or from source, without installing: `add_subdirectory(<path>/latch latch)` --
the same `latch::latch` target.

Tested through Rouster's selftest (the door's verb gate, undo, the manifest, the
source gate over latch's own sources). No GTK, no spdlog.
