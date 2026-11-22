# Bypass checks (silence)

Some archives ship a protection SDK that decides, inside its own code, whether
the process runs on the device it expects, and ends the process when it does
not. An emulated device is exactly what such an SDK refuses. The container
cannot fix that from the outside, because the SDK is working as designed.

"Bypass checks" is the per-app switch for this case. It tells the container to
stop calling the SDK's native half and to hold a static field at the value that
means "I am running".

It is a per-archive decision, not a general compatibility mode: it changes how
one package is started and nothing else.

## When to use it

The symptom is an app that exits by itself a few seconds after launch because
of its own protection library. For INCA GameGuard that library is
`libstub.so`. Both Crisis Fire packages in issues #72 and #73 die this way on
current builds:

- https://github.com/dima-xd/armbat/issues/72
- https://github.com/dima-xd/armbat/issues/73

Do not use it for every app that crashes: if the app does not fail inside its
own protection library, the entries do nothing.

## Opening the setting

1. Long-press the app icon in the library. The app card opens, the same card
   that LAUNCH and DELETE are on.
2. The last row of the card, above the buttons, is "Bypass checks" (the label
   follows the UI language). It shows the state of this package:
   - "Default" - no changes, the shipped entries apply (if any);
   - "Custom file" - the package has its own entries;
   - "Off" - no entries are applied.
3. Tap the row to edit the entries for this package only.

## Editing

The editor shows the entries of this package as plain text, one entry per line.

- Tap the text field to place the cursor and type. Start a new line for each
  entry.
- SAVE applies the text to this package. The next launch uses it.
- RESET removes the package's own entries and brings the shipped default back,
  if it has one.
- Clear the whole field and press SAVE to turn the entries off for this
  package.

Blank lines and lines starting with `#` are ignored, so a comment can sit next
to an entry.

## Entries

- A library or native method name, one per line, for example `libstub`. The
  container answers the call instead of running it, returning zero for any
  return type. The library is still loaded (`JNI_OnLoad` runs), otherwise the
  Java half would fail with `UnsatisfiedLinkError`.
- `flag:<class>#<field>=<value>` keeps a static field of the archive at a
  value. The container writes it before the archive's code starts, initializes
  the class itself (so `<clinit>` cannot overwrite it later) and restores it
  every 0.5 s if the archive changes it. Only `boolean` and `int` fields are
  supported.

Example:

```
# INCA GameGuard
libstub
flag:android.support.v4.security.coM5#true=true
```

## Shipped defaults

Two packages get entries without any changes:

```
com.crisisfire.cmge.classichug
com.crisisfire.android.uc
    libstub
    flag:android.support.v4.security.coM5#true=true
```

On current builds GameGuard kills both games about two seconds after launch;
the comment in issue #73 has the log. The default removes that layer only. It
does not fix the other blockers reported in #73: the UC SDK
`lib_ucsdk_softlink.so` namespace error and the missing `libandroid-support.so`
for the bundled server. If the app still exits with one of those, bypassing
checks is not the reason.

The defaults are data in the app, not a branch in the emulator, and they are
shown and edited by the same editor as user entries.

## Where the entries are kept

The editor writes `/sdcard/armbat/silence/<package>` for each package it
changes. The app card is the only place where these entries are seen and
changed.

Precedence:

- No edits for this package: the shipped default applies, if it has one.
- Edits exist: they are used as they are, even when the field was cleared and
  saved empty. An empty list means "no entries", which is how a shipped
  default is turned off.
- RESET removes this package's edits, so the shipped default comes back.

## Limits

- Entries apply at launch only. After SAVE, restart the app.
- Matches names in the guest loader. A library loaded through the host's
  `libnativeloader` (the UC SDK softlink in #73) is out of reach.
- A held field is a `boolean` or an `int`; strings and objects are not
  supported.
- It does not make an x86 or ARM64-only archive run, and it is not a substitute
  for a missing syscall or library.
