"""Small seeded CMA-ES (Hansen's (mu/mu_w, lambda) with rank-one + rank-mu updates), box-bounded.

Works in the normalised box [0, 1]^n; samples are clipped into the box (the clipped point is what is evaluated and
told back). Deterministic given ``seed``; no wall-clock or global RNG use."""
from __future__ import annotations

import numpy as np


class CMAES:
    def __init__(self, x0, sigma0: float = 0.25, popsize: int | None = None, seed: int = 0):
        self.n = n = len(x0)
        self.rng = np.random.default_rng(seed)
        self.lam = popsize or (4 + int(3 * np.log(n)))
        self.mu = self.lam // 2
        w = np.log(self.mu + 0.5) - np.log(np.arange(1, self.mu + 1))
        self.w = w / w.sum()
        self.mueff = 1.0 / np.sum(self.w ** 2)
        self.cc = (4 + self.mueff / n) / (n + 4 + 2 * self.mueff / n)
        self.cs = (self.mueff + 2) / (n + self.mueff + 5)
        self.c1 = 2 / ((n + 1.3) ** 2 + self.mueff)
        self.cmu = min(1 - self.c1, 2 * (self.mueff - 2 + 1 / self.mueff) / ((n + 2) ** 2 + self.mueff))
        self.damps = 1 + 2 * max(0, np.sqrt((self.mueff - 1) / (n + 1)) - 1) + self.cs
        self.chiN = np.sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n))
        self.mean = np.clip(np.asarray(x0, float), 0, 1)
        self.sigma = sigma0
        self.pc = np.zeros(n)
        self.ps = np.zeros(n)
        self.B = np.eye(n)
        self.D = np.ones(n)
        self.C = np.eye(n)
        self.gen = 0
        self.best_x, self.best_f = self.mean.copy(), np.inf

    def ask(self) -> np.ndarray:
        z = self.rng.standard_normal((self.lam, self.n))
        y = (z * self.D[None, :]) @ self.B.T
        X = np.clip(self.mean[None, :] + self.sigma * y, 0.0, 1.0)
        return X

    def tell(self, X: np.ndarray, f) -> None:
        f = np.asarray(f, float)
        order = np.argsort(f, kind="stable")
        if f[order[0]] < self.best_f:
            self.best_f, self.best_x = float(f[order[0]]), X[order[0]].copy()
        Xs = X[order[: self.mu]]
        old = self.mean
        self.mean = self.w @ Xs
        y = (self.mean - old) / self.sigma
        invsqrtC = self.B @ np.diag(1.0 / self.D) @ self.B.T
        self.ps = (1 - self.cs) * self.ps + np.sqrt(self.cs * (2 - self.cs) * self.mueff) * (invsqrtC @ y)
        hsig = (np.linalg.norm(self.ps) / np.sqrt(1 - (1 - self.cs) ** (2 * (self.gen + 1))) / self.chiN
                < 1.4 + 2 / (self.n + 1))
        self.pc = (1 - self.cc) * self.pc + hsig * np.sqrt(self.cc * (2 - self.cc) * self.mueff) * y
        ys = (Xs - old[None, :]) / self.sigma
        self.C = ((1 - self.c1 - self.cmu) * self.C
                  + self.c1 * (np.outer(self.pc, self.pc) + (1 - hsig) * self.cc * (2 - self.cc) * self.C)
                  + self.cmu * (ys.T * self.w) @ ys)
        self.sigma *= np.exp((self.cs / self.damps) * (np.linalg.norm(self.ps) / self.chiN - 1))
        self.sigma = float(min(self.sigma, 1.0))
        self.C = (self.C + self.C.T) / 2
        d2, self.B = np.linalg.eigh(self.C)
        self.D = np.sqrt(np.maximum(d2, 1e-20))
        self.gen += 1


def minimize(fn, x0, sigma0=0.25, popsize=None, generations=20, seed=0, evaluate_batch=None):
    """Run CMA-ES. ``fn(x) -> float`` or ``evaluate_batch(X) -> list[float]`` (parallel evaluation, order kept).
    Returns (best_x, best_f, history[list of best-so-far per generation]). The initial point is evaluated first
    so the result is never worse than x0."""
    es = CMAES(x0, sigma0, popsize, seed)
    batch = evaluate_batch or (lambda X: [fn(x) for x in X])
    f0 = batch(np.asarray([es.mean]))[0]
    es.best_f, es.best_x = float(f0), es.mean.copy()
    hist = [es.best_f]
    for _ in range(generations):
        X = es.ask()
        es.tell(X, batch(X))
        hist.append(es.best_f)
    return es.best_x, es.best_f, hist
