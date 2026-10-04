"""How much of the exported graph could the CoreML EP (MLProgram) take? (runs in the venv, Linux is fine)

usage: coreml_coverage.py <model.onnx> [more.onnx ...]

ORT hands the CoreML EP the graph after the level-1 ("basic": constant folding, shape folding, identity
elimination, ...) optimisations, so we dump that graph and count its op types against the
`MLProgram` operator table of the CoreML EP docs (docs/execution-providers/CoreML-ExecutionProvider.md,
onnxruntime gh-pages, retrieved 2026-10-04). Static analysis only: constraints in the table's Note column
(constant weights, 4D, ...) are NOT checked, so this is an upper bound on the supported node count.
"""
import collections, sys, tempfile, os
import onnx, onnxruntime as ort

MLPROGRAM = set("""Add Argmax AveragePool Cast Clip Concat Conv ConvTranspose DepthToSpace Div Erf Gemm Gelu GlobalAveragePool
GlobalMaxPool GridSample GroupNormalization InstanceNormalization LayerNormalization LeakyRelu MatMul MaxPool Max Mul Pow
PRelu Reciprocal ReduceSum ReduceMean ReduceMax Relu Reshape Resize Round Shape Slice Split Sub Sigmoid Softmax Sqrt
Squeeze Tanh Transpose Unsqueeze""".split())

for path in sys.argv[1:]:
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    out = os.path.join(tempfile.mkdtemp(), "basic.onnx")
    so.optimized_model_filepath = out
    ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
    g = onnx.load(out, load_external_data=False).graph
    ops = collections.Counter(n.op_type for n in g.node)
    tot = sum(ops.values()); ok = sum(c for o, c in ops.items() if o in MLPROGRAM)
    print(f"\n{os.path.basename(path)}: {tot} nodes after basic optimisation; {ok} ({100*ok/tot:.0f}%) have an MLProgram op builder")
    print("  not in the MLProgram table:", ", ".join(f"{o} x{c}" for o, c in ops.most_common() if o not in MLPROGRAM))
    print("  in the table:", ", ".join(f"{o} x{c}" for o, c in ops.most_common() if o in MLPROGRAM))
    # split the node list into maximal runs of supported nodes: each run boundary is a CoreML <-> CPU partition edge
    runs, cur = 0, False
    for n in g.node:
        s = n.op_type in MLPROGRAM
        if s and not cur: runs += 1
        cur = s
    print(f"  maximal runs of consecutive supported nodes in topological order (upper bound on partition count): {runs}")
