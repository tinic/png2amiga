#!/usr/bin/env python3
"""Fit minimax polynomials for srgb_to_linear(x) and pow(x, 2.4) on [0, 1].

Tries multiple degrees, reports max abs error vs the analytic reference.
Outputs C-style coefficient arrays ready to drop into a Horner FMA chain.

The dither inner loop uses srgb_to_linear(target_s) where target_s ∈ [0, 1]
in practice (clamped). A direct polynomial fit replaces SLEEF's xlogf+xexpf
chain (which has a vdivps + ~25 ops per call) with a single Horner chain
(~9-10 ops, no division).
"""
import numpy as np
from numpy.polynomial import chebyshev as ch


def srgb_to_linear(x):
    return np.where(x <= 0.04045,
                     x / 12.92,
                     ((x + 0.055) / 1.055) ** 2.4)


def pow24(x):
    # The bare pow(x, 2.4) part of srgb_to_linear (no piecewise branch).
    return ((x + 0.055) / 1.055) ** 2.4


def remez_fit(f, a, b, degree, n_samples=8192, n_iter=20):
    """Quick Remez-ish minimax fit. Iteratively reweights against the
    residual until max-abs-error stabilises."""
    xs = np.linspace(a, b, n_samples)
    ys = f(xs)
    weights = np.ones_like(xs)
    coeffs = None
    for _ in range(n_iter):
        coeffs = np.polynomial.polynomial.polyfit(xs, ys, degree, w=weights)
        residual = ys - np.polynomial.polynomial.polyval(xs, coeffs)
        # Up-weight high-error regions for the next pass.
        weights = 1.0 / (np.abs(residual) + 1e-9)
        weights /= weights.mean()
    pred = np.polynomial.polynomial.polyval(xs, coeffs)
    max_abs = np.max(np.abs(pred - ys))
    # Relative error only where reference > 1e-6 (avoid div by tiny).
    nz = ys > 1e-6
    max_rel = np.max(np.abs(pred[nz] - ys[nz]) / ys[nz]) if nz.any() else 0
    return coeffs, max_abs, max_rel


def chebyshev_fit(f, a, b, degree, n_samples=8192):
    """Chebyshev least-squares fit, converted to monomial coefficients."""
    xs = np.linspace(a, b, n_samples)
    ys = f(xs)
    cseries = ch.Chebyshev.fit(xs, ys, degree)
    coeffs = cseries.convert(kind=np.polynomial.Polynomial).coef
    pred = np.polyval(coeffs[::-1], xs)
    max_abs = np.max(np.abs(pred - ys))
    nz = ys > 1e-6
    max_rel = np.max(np.abs(pred[nz] - ys[nz]) / ys[nz]) if nz.any() else 0
    return coeffs, max_abs, max_rel


def report(label, coeffs, max_abs, max_rel):
    print(f"\n--- {label} (degree {len(coeffs)-1}) ---")
    print(f"  max abs err: {max_abs:.3e}")
    print(f"  max rel err: {max_rel:.3e}")
    print("  coefficients (lowest degree first):")
    for i, c in enumerate(coeffs):
        print(f"    c{i} = {c:+.10e}f,  // x^{i}")


print("=== srgb_to_linear(x) on [0, 1] ===")
for d in (7, 9, 11, 13):
    c, ma, mr = chebyshev_fit(srgb_to_linear, 0, 1, d)
    print(f"  Chebyshev d={d}: abs={ma:.3e}, rel={mr:.3e}")

# Final pick: best degree under 1e-4 abs, with smallest count
print()
print("=== Best fit reports ===")
for d in (9, 11, 13):
    c, ma, mr = chebyshev_fit(srgb_to_linear, 0, 1, d)
    report(f"chebyshev srgb_to_linear", c, ma, mr)

# Also try: bare pow(x, 2.4) (no linear branch). Used inside the
# blendv pattern instead of fitting the whole composite function.
print()
print("=== pow24((x+0.055)/1.055) on [0, 1] (no linear branch) ===")
for d in (7, 9, 11):
    c, ma, mr = chebyshev_fit(pow24, 0, 1, d)
    print(f"  Chebyshev d={d}: abs={ma:.3e}, rel={mr:.3e}")

c, ma, mr = chebyshev_fit(pow24, 0, 1, 9)
report("chebyshev pow24", c, ma, mr)
