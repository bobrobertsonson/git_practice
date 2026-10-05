"""Separation model files: fetch the official checkpoints, export the ONNX core, verify, hash.

`sawblade-models fetch` is the tooling behind the C++ `ModelStore` (docs/specs/phase5_1b_separator_core.md).
Everything that needs torch/onnx is imported lazily; paths, hashing, download and status work without them.
"""
