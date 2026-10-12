# Mac 1 fix: clang -Werror build break (macOS)

Branch: `claude/sawblade-mac-build`. Base: `origin/claude/sawblade-plugin-setup-7k0b8q` @ `435838a`.

## Problem

On macOS (Apple clang 21, arm64), `cmake --build build-mac` (Release, `-DSAWBLADE_BUILD_PLUGIN=ON`)
fails:

```
core/include/sawblade/stem_player.h:198:10: error: private field 'master_' is not used [-Werror,-Wunused-private-field]
```

`-Wunused-private-field` is a clang-only warning; GCC on Linux does not emit it, so the Linux build
is green. `StemPlayer::master_` is never read or written; the master level is `masterDb_` +
`masterRamp_`.

## Task

Make the Sawblade targets build warning-free under Apple clang with the existing
`-Wall -Wextra -Wpedantic -Werror` flags.

1. Find **every** clang-only diagnostic, not just the first: build with `ninja -k 0` (or
   `cmake --build build-mac -- -k 0`) and fix all errors on Sawblade targets.
2. Fix each at the source with the smallest change that does not alter behaviour. For
   `master_`: delete the unused member.
3. Do **not** change any DSP behaviour or output, do not weaken warning flags, and do not add
   `-Wno-…` or pragmas, unless a diagnostic cannot be fixed at the source (then explain why under
   "Decisions / questions for lead").
4. Out of scope: the known failing test `plugin:Processor: starts as a zero-latency pass-through
   (Init preset)` (FMA parameter round-trip; awaiting a user decision). Leave it failing.

## Acceptance

- `cmake --build build-mac` succeeds with zero compiler warnings on Sawblade targets.
- `ctest --test-dir build-mac --output-on-failure`: every test passes except the known Init
  pass-through failure and the Linux-only LockGuard skip.
- The diff touches only the lines needed to remove the diagnostics; it contains no functional
  change (the reviewer checks that each removed/changed symbol had no reads or writes).
