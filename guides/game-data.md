# Game data: expansion files and caches

A game is rarely one file. Next to the archive there is usually an expansion
file (`main.<version>.<package>.obb`, sometimes a `patch` too) or a cache folder
the game reads on first launch, and until now the way to place it was to put it
into `/sdcard/armbat/android/obb/<package>/` with a file manager.

On Android 11 and newer that road is closed for a different reason than before:
`Android/data` and `Android/obb` are hidden from every file manager and from the
system picker. On a device without a 32-bit ABI, a game's own expansion file
therefore cannot be placed by hand at all.

The SD-card icon in the dock takes both shapes and places them itself.

## Expansion files

Pick one or both `.obb` files. The name is enough to know everything:

- the package is the third part of `main.<version>.<package>.obb`;
- the game has to be in the library already, otherwise there is nothing to
  attach the file to;
- the version in the name is **not** kept. A game composes the path it opens
  from its own version code, and the container reports the code of the archive
  you imported; the copy is named after that code. An expansion file whose name
  still carries another version is a file the game will never open.
- an older expansion file of the same kind is removed. The game looks up one
  file by its version code; a second copy is dead weight, not a spare.

## Cache archives

Pick a `.zip`. The panel asks one question -- where the archive belongs:

| Destination | What lands there |
|---|---|
| As packed | the container's tree, keeping `Android/obb/<package>/` and `Android/data/<package>/` prefixes intact, which is how a cache is usually shipped |
| Internal files | `<data>/files`, the game's `getFilesDir()` |
| External files | `<data>/external/files`, the game's `getExternalFilesDir()` |
| Data folder | `<data>`, for an archive that starts with `files/`, `cache/` or `shared_prefs/` |

"As packed" needs no package at all: the archive's own paths already name it.
The other three are addresses inside a game, so the game is the second half of
that address: it is asked right after such a destination is picked, never on the
first screen, and the archive's name answers it without a question when it
carries a package that is in the library.

Only `.zip` is accepted. `.rar` and `.7z` are not: each needs its own decoder,
and both are rare for game caches.

## What happens on disk

- The copy goes through a `.part` file in the destination folder and is renamed
  when it is whole, so a game never opens a half-written expansion file.
- The panel shows the percentage and can be stopped with its own button or the
  back key; a stopped copy leaves nothing behind.
- Free space is checked before the copy starts, with a margin, because a volume
  filled to the last byte fails in the middle of the write.
- Nothing is written outside the destination: archive entries that name `..`, an
  absolute path or a Windows separator are skipped, and the resolved path is
  checked against the destination root as well.

## When a file does not fit through the picker

Two files are already there and the picker is the wrong tool for a 20 GB dump:
`adb` writes into the same tree without any grant.

```bash
adb push main.1.com.example.obb \
  /sdcard/armbat/android/obb/com.example/
```

The paths the guest sees are mapped onto the container's tree, so a file placed
by hand and a file placed by the picker are the same file.
