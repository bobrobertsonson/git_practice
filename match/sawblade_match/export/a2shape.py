"""Python port of the pinned core's ``nam::wavenet::a2_fast::is_a2_shape`` (NeuralAmpModelerCore ``0b3d3c97``,
``NAM/wavenet/a2_fast.cpp:832-978``, constants ``a2_fast.h:30-41``): which standalone WaveNet configs take the A2 fast path
(``NAM_ENABLE_A2_FAST``).  Anything else plays correctly through the generic WaveNet, only slower.

Used to report (``export_report.json`` ``a2FastPath``) and to test that the exported A2 Full / A2 Lite files have the shape
the core optimises.  It is a detector port for checking, not a second player: no DSP here.
"""
from __future__ import annotations

NUM_LAYERS = 23
HEAD_KERNEL = 16
LEAKY_SLOPE = 0.01
KERNEL_SIZES = [6] * 14 + [15, 15] + [6] * 7
DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
FILM_KEYS = ("conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
             "activation_pre_film", "activation_post_film", "layer1x1_post_film", "head1x1_post_film")


def _int(x) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def _film_inactive(la: dict, key: str) -> bool:
    v = la.get(key)
    if v is None or v is False:
        return True
    if isinstance(v, list):
        return all(_film_inactive({"k": e}, "k") for e in v)
    if isinstance(v, dict):
        return not v.get("active", False)
    return False


def a2_channels(config: dict) -> int | None:
    """3 (A2 Lite) or 8 (A2 Full) when ``config`` (a standalone WaveNet ``config`` object) has the A2 fast-path shape, else
    ``None``."""
    layers = config.get("layers")
    if not isinstance(layers, list) or len(layers) != 1:
        return None
    if config.get("head") is not None or config.get("condition_dsp") is not None:
        return None
    if not isinstance(config.get("head_scale"), (int, float)) or isinstance(config.get("head_scale"), bool):
        return None
    if config.get("in_channels", 1) != 1:
        return None
    la = layers[0]
    if la.get("input_size", 0) != 1 or la.get("condition_size", 0) != 1:
        return None
    ch = la.get("channels", 0)
    if ch != la.get("bottleneck", 0) or ch not in (3, 8):
        return None
    ks, dl, act = la.get("kernel_sizes"), la.get("dilations"), la.get("activation")
    if not (isinstance(ks, list) and len(ks) == NUM_LAYERS and all(_int(k) for k in ks) and ks == KERNEL_SIZES):
        return None
    if not (isinstance(dl, list) and len(dl) == NUM_LAYERS and all(_int(k) for k in dl) and dl == DILATIONS):
        return None
    if not (isinstance(act, list) and len(act) == NUM_LAYERS):
        return None
    for a in act:
        if not isinstance(a, dict) or a.get("type") != "LeakyReLU" or abs(float(a.get("negative_slope", 0.0)) - LEAKY_SLOPE) > 1e-6:
            return None
    gm = la.get("gating_mode")
    if gm is not None and (len(gm) != NUM_LAYERS or any(g != "none" for g in gm)):
        return None
    if la.get("gated") is True:
        return None
    sa = la.get("secondary_activation")
    if sa is not None and (len(sa) != NUM_LAYERS or any(v is not None for v in sa)):
        return None
    h1 = la.get("head1x1")
    if isinstance(h1, dict) and h1.get("active", False):
        return None
    l1 = la.get("layer1x1")
    if not isinstance(l1, dict) or not l1.get("active", False) or l1.get("groups", 1) != 1:
        return None
    head = la.get("head")
    if not isinstance(head, dict) or head.get("out_channels", 0) != 1 or head.get("kernel_size", 0) != HEAD_KERNEL:
        return None
    if head.get("head_dilation", 1) != 1 or not head.get("bias", False):
        return None
    if not all(_film_inactive(la, k) for k in FILM_KEYS):
        return None
    if la.get("groups_input", 1) != 1 or la.get("groups_input_mixin", 1) != 1:
        return None
    if la.get("slimmable") is not None:
        return None
    return int(ch)


def nam_file_fast_path(model: dict) -> int | None:
    """Channels (3 / 8) when the parsed ``.nam`` is a standalone WaveNet taking the A2 fast path, else ``None`` (a
    ``SlimmableContainer`` is never taken as a whole: each of its submodels is built separately by the core)."""
    if model.get("architecture") != "WaveNet":
        return None
    return a2_channels(model.get("config") or {})
