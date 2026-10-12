# Phase 4.1: resumable NAM export training

The cloud container restarts without warning, and one restart killed an overnight CPU
training run at epoch 38 of 250. Training must survive that.

## Changes (match-engineer, export/ only)
1. `sawblade-export` writes a checkpoint after every epoch to `<out>/checkpoint/`:
   - the Lightning checkpoint (model, optimizer, scheduler, epoch, RNG states);
   - the best-so-far model;
   - `progress.json` with the epoch, best val ESR, elapsed training seconds, and the
     signal and preset sha256.

   Checkpoints are written atomically (write to a temp file, then rename).
2. `--resume <out dir>` continues from the last checkpoint. It refuses with a clear message if
   the preset sha, signal sha, size or mode differ. The wall-time cap (`--max-minutes`) counts
   total elapsed training time across resumes.
3. `--resume auto`: if a matching unfinished run exists in the exports dir for the same preset,
   mode and size, resume it; otherwise start fresh. Print which one happened.
4. Remove the checkpoint dir after a successful export unless `--keep-scratch` is given.
5. **Tests:**
   - resume after 2 epochs gives the same weights as an uninterrupted 4-epoch run (bit-identical
     on CPU with the same thread count; otherwise ESR within 1e-6);
   - a sha mismatch is refused;
   - `--resume auto` picks the right directory.

   Use tiny feather runs under `SAWBLADE_TEST_TRAIN`, plus an always-on test with a mocked
   trainer.
