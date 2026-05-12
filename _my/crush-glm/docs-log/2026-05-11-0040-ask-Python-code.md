
My Python project code (it's from Trader7 project which is comletelly separate project from my current C++ project) for robustnes detection, check if that was done correctly and explain what it does and how does it correlated to what we have discussed:

```py
"""
pipeline/surface.py — Phase 3: surface analysis on grid results.

Functions:
  detect_plateau_zone()     — find contiguous good regions per param axis
  detect_spike()            — reject isolated peaks
  detect_multidim_clusters() — find non-axis-aligned plateaus
  export_surface_artifacts() — save heatmaps + topology JSON
"""
from __future__ import annotations
from dataclasses import dataclass, field
from html import escape
from pathlib import Path
import json
import numpy as np
import pandas as pd

from pipeline.auto_analysis.stats import distribution_stats


@dataclass
class PlateauZone:
    param_name:  str
    param_values: list
    mean_sharpe: float
    width:       int           # number of grid steps spanning the plateau
    bounds:      tuple         # (low_value, high_value)
    center:      object


@dataclass
class ZoneTopology:
    plateaus:   list[PlateauZone] = field(default_factory=list)
    spikes:     list[dict]        = field(default_factory=list)
    best_combo: dict              = field(default_factory=dict)
    plateau_found: bool           = False


def _coerce_axis_value(value):
    """Return a JSON-friendly axis value without forcing categories to floats."""
    if isinstance(value, (np.integer, int)) and not isinstance(value, bool):
        return int(value)
    if isinstance(value, (np.floating, float)):
        return float(value)
    if pd.isna(value):
        return None
    return value


def _axis_sort_key(value) -> tuple[int, float | str]:
    """Sort numeric-looking grid labels numerically, then categorical labels."""
    if pd.isna(value):
        return (2, "")
    if isinstance(value, bool):
        return (0, float(value))
    numeric = pd.to_numeric(pd.Series([value]), errors="coerce").iloc[0]
    if pd.notna(numeric):
        return (0, float(numeric))
    return (1, str(value))


def _sort_grid_frame(frame: pd.DataFrame, param: str) -> pd.DataFrame:
    """Sort one-param grouped output with mixed numeric/categorical values."""
    return (
        frame.assign(_axis_sort_key=frame[param].map(_axis_sort_key))
        .sort_values("_axis_sort_key")
        .drop(columns=["_axis_sort_key"])
    )


def _sorted_unique(values) -> list:
    """Return deterministic unique values for numeric or categorical axes."""
    return sorted(pd.Series(values).drop_duplicates().tolist(), key=_axis_sort_key)


def _axis_center(values: list):
    """
    Return a sensible plateau center for numeric axes and a label for categories.

    Older code used `np.mean()` for every axis, which crashes for regime labels
    and silently assigns fake numeric meaning to categories.  Numeric-looking
    labels still get a numeric center; categorical plateaus get their middle
    grid label as the representative center.
    """
    if not values:
        return None
    numeric = pd.to_numeric(pd.Series(values), errors="coerce")
    if numeric.notna().all():
        return float(numeric.mean())
    return _coerce_axis_value(values[len(values) // 2])


def _cluster_features(frame: pd.DataFrame, valid_params: list[str]) -> tuple[pd.DataFrame, dict[str, str]]:
    """Build numeric clustering features while preserving categorical axes."""
    features = pd.DataFrame(index=frame.index)
    kinds: dict[str, str] = {}
    for param in valid_params:
        numeric = pd.to_numeric(frame[param], errors="coerce")
        if numeric.notna().all() and numeric.nunique(dropna=True) > 1:
            features[param] = numeric.astype(float)
            kinds[param] = "numeric"
            continue
        codes, _ = pd.factorize(frame[param].astype(str), sort=True)
        features[param] = codes.astype(float)
        kinds[param] = "categorical"
    return features, kinds


def _centroid_value(members: pd.DataFrame, param: str, kind: str):
    """Return mean for numeric axes and mode for categorical axes."""
    if kind == "numeric":
        return float(pd.to_numeric(members[param], errors="coerce").mean())
    mode = members[param].mode(dropna=False)
    if mode.empty:
        return None
    return _coerce_axis_value(mode.iloc[0])


def detect_plateau_zone(
    results_df:    pd.DataFrame,
    param_names:   list[str],
    metric:        str   = "sharpe_ratio",
    min_sharpe:    float = 0.3,
    min_width:     int   = 3,
) -> ZoneTopology:
    """
    For each param axis, identify contiguous regions where mean metric >= min_sharpe
    and spanning at least min_width grid steps.
    """
    topo = ZoneTopology()

    if results_df.empty or metric not in results_df.columns:
        return topo

    # Best combo overall
    best_idx = results_df[metric].idxmax()
    topo.best_combo = results_df.loc[best_idx].to_dict()

    for param in param_names:
        if param not in results_df.columns:
            continue
        grouped_df = results_df.groupby(param, dropna=False)[metric].mean().reset_index(name=metric)
        grouped_df = _sort_grid_frame(grouped_df, param)
        values  = grouped_df[param].tolist()
        scores  = pd.to_numeric(grouped_df[metric], errors="coerce").fillna(-np.inf).to_numpy()

        # Find contiguous runs above threshold
        above = scores >= min_sharpe
        runs  = []
        start = None
        for i, a in enumerate(above):
            if a and start is None:
                start = i
            elif not a and start is not None:
                runs.append((start, i - 1))
                start = None
        if start is not None:
            runs.append((start, len(above) - 1))

        for s, e in runs:
            width = e - s + 1
            if width >= min_width:
                plateau_vals = [values[i] for i in range(s, e + 1)]
                plateau_scores = scores[s:e+1]
                topo.plateaus.append(PlateauZone(
                    param_name   = param,
                    param_values = plateau_vals,
                    mean_sharpe  = float(plateau_scores.mean()),
                    width        = width,
                    bounds       = (plateau_vals[0], plateau_vals[-1]),
                    center       = _axis_center(plateau_vals),
                ))
                topo.plateau_found = True

    return topo


def detect_spike(
    results_df:   pd.DataFrame,
    param_names:  list[str],
    metric:       str   = "sharpe_ratio",
    drop_ratio:   float = 0.5,
) -> list[dict]:
    """
    Check whether the BEST point is an isolated spike.

    A spike is where the top-ranked combo has ALL immediate neighbours
    degraded by more than drop_ratio × peak_value. We only check the
    best point — good points at the edge of a plateau having worse
    neighbours is expected and should not be flagged.

    Returns a list with one entry if the best point is a spike, else empty.
    """
    if results_df.empty:
        return []

    valid_params = [p for p in param_names if p in results_df.columns]
    if not valid_params:
        return []

    # Build lookup
    lookup = {}
    for _, row in results_df.iterrows():
        key = tuple(row[p] for p in valid_params)
        lookup[key] = float(row[metric])

    grid_values = {p: _sorted_unique(results_df[p].unique().tolist()) for p in valid_params}

    # Only check the single best point
    best_idx = results_df[metric].idxmax()
    best_row = results_df.loc[best_idx]
    best_key = tuple(best_row[p] for p in valid_params)
    best_score = lookup[best_key]

    if best_score <= 0:
        return []

    neighbour_scores = []
    for dim_i, param in enumerate(valid_params):
        ordered = grid_values[param]
        cur_val = best_row[param]
        if cur_val not in ordered:
            continue
        idx = ordered.index(cur_val)
        for delta in (-1, +1):
            ni = idx + delta
            if 0 <= ni < len(ordered):
                nb_key = list(best_key)
                nb_key[dim_i] = ordered[ni]
                nb_score = lookup.get(tuple(nb_key))
                if nb_score is not None:
                    neighbour_scores.append(nb_score)

    if not neighbour_scores:
        return []

    threshold = best_score * (1 - drop_ratio)
    is_spike = all(s < threshold for s in neighbour_scores)

    if is_spike:
        return [{
            "params":           dict(zip(valid_params, best_key)),
            "score":            best_score,
            "median_neighbour": float(np.median(neighbour_scores)),
            "is_spike":         True,
        }]
    return []


def detect_multidim_clusters(
    results_df:  pd.DataFrame,
    param_names: list[str],
    metric:      str   = "sharpe_ratio",
    min_sharpe:  float = 0.5,
    n_clusters:  int   = 3,
) -> list[dict]:
    """
    Simple k-means-style clustering of good combos in param space.
    Returns top n_clusters cluster centroids.
    """
    good = results_df[results_df[metric] >= min_sharpe].copy()
    if len(good) < n_clusters:
        return []

    valid_params = [p for p in param_names if p in good.columns]
    feature_df, feature_kinds = _cluster_features(good, valid_params)
    if feature_df.empty:
        return []
    X = feature_df.values.astype(float)

    # Normalise
    X_norm = (X - X.mean(axis=0)) / (X.std(axis=0) + 1e-9)

    try:
        from sklearn.cluster import KMeans
        km = KMeans(n_clusters=min(n_clusters, len(good)), random_state=42, n_init=10)
        labels = km.fit_predict(X_norm)
        good = good.copy()
        good["_cluster"] = labels

        clusters = []
        for cid in range(km.n_clusters):
            members = good[good["_cluster"] == cid]
            clusters.append({
                "cluster_id": cid,
                "size":       len(members),
                "mean_sharpe": float(members[metric].mean()),
                "centroid":   {p: _centroid_value(members, p, feature_kinds.get(p, "categorical")) for p in valid_params},
            })
        return sorted(clusters, key=lambda c: c["mean_sharpe"], reverse=True)
    except ImportError:
        return []
    except (TypeError, ValueError):
        return []


def export_surface_artifacts(
    results_df:  pd.DataFrame,
    param_names: list[str],
    out_dir:     Path,
    topology:    ZoneTopology | None = None,
    metric:      str = "sharpe_ratio",
    write_svgs:  bool = True,
) -> None:
    """
    Save:
      - topology.json  (plateau/spike summary)
      - heatmap CSVs per param pair (for external plotting)
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    valid_params = [p for p in param_names if p in results_df.columns]

    # topology JSON
    if topology:
        topo_dict = {
            "plateau_found": topology.plateau_found,
            "plateaus": [
                {
                    "param":    p.param_name,
                    "bounds":   list(p.bounds),
                    "width":    p.width,
                    "mean_sharpe": p.mean_sharpe,
                    "center":   p.center,
                }
                for p in topology.plateaus
            ],
            "n_spikes": len(topology.spikes),
            "best_combo": {
                k: (float(v) if isinstance(v, (np.floating, float)) else
                    int(v)   if isinstance(v, (np.integer, int))    else v)
                for k, v in topology.best_combo.items()
                if not k.startswith("_")
            },
        }
        with open(out_dir / "30-topology.json", "w") as f:
            json.dump(topo_dict, f, indent=2)

    # Heatmap CSVs for every param pair
    for i, p1 in enumerate(valid_params):
        for p2 in valid_params[i+1:]:
            pivot = results_df.groupby([p1, p2])[metric].mean().unstack(p2)
            csv_path = out_dir / f"31-heatmap_{p1}_vs_{p2}.csv"
            pivot.to_csv(csv_path)
            if write_svgs:
                _write_heatmap_svg(
                    pivot,
                    csv_path.with_suffix(".svg"),
                    title=f"{metric} surface: {p1} vs {p2}",
                    x_label=p2,
                    y_label=p1,
                    metric=metric,
                )

    # Summary CSV
    cols_to_save = valid_params + [
        "sharpe_ratio", "total_return", "max_drawdown",
        "calmar_ratio", "n_trades", "win_rate", "profit_factor",
    ]
    cols_to_save = [c for c in cols_to_save if c in results_df.columns]
    results_df[cols_to_save].to_csv(out_dir / "32-grid_results.csv", index=False)


def export_surface_svg_charts(
    out_dir: Path,
    metric: str = "sharpe_ratio",
    chart_metadata: dict | None = None,
) -> list[Path]:
    """
    Render SVG heatmaps for existing `31-heatmap_*.csv` files.

    Older runs may have CSV surfaces but no charts. Auto-analysis calls this
    helper so refreshing a historical run upgrades it without rerunning VBT.
    """
    out_dir = Path(out_dir)
    written: list[Path] = []
    for csv_path in sorted(out_dir.rglob("31-heatmap_*_vs_*.csv")):
        try:
            pivot = pd.read_csv(csv_path, index_col=0)
            stem = csv_path.stem.replace("31-heatmap_", "")
            y_label, x_label = stem.split("_vs_", 1)
            svg_path = csv_path.with_suffix(".svg")
            _write_heatmap_svg(
                pivot,
                svg_path,
                title=f"{metric} surface: {y_label} vs {x_label}",
                x_label=x_label,
                y_label=y_label,
                metric=metric,
                chart_metadata=chart_metadata,
            )
            written.append(svg_path)
        except Exception:
            continue
    return written


def export_robustness_charts(
    results_df: pd.DataFrame,
    param_names: list[str],
    out_dir: Path,
    min_trades_threshold: int = 50,
    metric: str = "sharpe_ratio",
    chart_metadata: dict | None = None,
) -> list[Path]:
    """
    Write the non-heatmap sweep charts required for robust edge review.

    These SVGs are intentionally dependency-free so every run folder remains
    portable. They complement the CSV/JSON artifacts; they are not the source of
    truth, but they let a beginner spot spikes and trade-count problems quickly.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    if results_df.empty or metric not in results_df.columns:
        return written

    for param in [p for p in param_names if p in results_df.columns]:
        path = out_dir / f"31-plateau_profile_{param}.svg"
        grouped = results_df.groupby(param, dropna=False).agg(
            mean_score=(metric, "mean"),
            median_score=(metric, "median"),
            p25_score=(metric, lambda s: s.quantile(0.25)),
            p75_score=(metric, lambda s: s.quantile(0.75)),
            min_score=(metric, "min"),
            max_score=(metric, "max"),
        ).reset_index()
        grouped = _sort_grid_frame(grouped, param)
        path.write_text(_render_plateau_profile_svg(grouped, param, metric, chart_metadata), encoding="utf-8")
        written.append(path)

    distribution_path = out_dir / "31-sharpe_distribution.svg"
    distribution_path.write_text(_render_distribution_svg(results_df[metric], metric, chart_metadata), encoding="utf-8")
    written.append(distribution_path)

    scatter_path = out_dir / "31-trade_count_vs_sharpe.svg"
    scatter_path.write_text(
        _render_trade_count_scatter_svg(results_df, min_trades_threshold, metric, chart_metadata),
        encoding="utf-8",
    )
    written.append(scatter_path)
    return written


"""
pipeline/robustness.py — Phase 4: inline robustness tests.
All tests run on already-computed grid results — zero new backtests.

Functions:
  compute_pbo_cscv()       — combinatorial symmetric cross-validation
  compute_mc_bootstrap()   — block bootstrap Monte Carlo
  compute_dsr()            — deflated Sharpe ratio
  compute_neighborhood()   — ±1 step neighbour degradation scoring
  score_candidates()       — composite robustness score
"""
from __future__ import annotations
import itertools
from typing import Any
import numpy as np
import pandas as pd
from scipy import stats


# ─────────────────────────────────────────────
# PBO via CSCV (Bailey et al.)
# ─────────────────────────────────────────────

def compute_pbo_cscv(
    equity_curves: list[pd.Series],
    n_splits: int = 16,
    max_combinations: int = 512,
) -> dict:
    """
    Probability of backtest overfitting via Combinatorial Symmetric CV.
    equity_curves: list of equity Series for the top plateau candidates.
    Returns: {"pbo": float, "pass": bool}
    """
    if not equity_curves or len(equity_curves) < 4:
        return {"pbo": 0.5, "pass": False, "unavailable": True, "note": "insufficient_curves"}

    # Align all equity curves
    try:
        rets = pd.concat(
            [eq.pct_change().dropna().rename(i) for i, eq in enumerate(equity_curves)],
            axis=1
        ).dropna()
    except Exception:
        return {"pbo": 0.5, "pass": False, "unavailable": True, "note": "alignment_error"}

    if len(rets) < n_splits * 2:
        return {"pbo": 0.5, "pass": False, "unavailable": True, "note": "too_short"}

    values = rets.to_numpy(dtype=float)
    T = len(values)
    S = n_splits
    fold_size = T // S

    # Generate all S/2 C(S, S/2) train-test splits
    split_indices = list(range(S))
    half = S // 2
    split_combos = list(itertools.combinations(split_indices, half))
    n_splits_possible = len(split_combos)
    if max_combinations and len(split_combos) > max_combinations:
        # Deterministic down-sampling keeps runtime bounded while preserving
        # broad coverage of the symmetric folds.  Full C(16, 8)=12,870 PBO is
        # too expensive for interactive full-mode research grids.
        picks = np.linspace(0, len(split_combos) - 1, max_combinations, dtype=int)
        split_combos = [split_combos[int(i)] for i in picks]

    logit_values = []
    for is_folds in split_combos:
        oos_folds = [f for f in split_indices if f not in is_folds]

        # IS performance
        is_idx = np.concatenate([np.arange(f * fold_size, (f+1) * fold_size) for f in is_folds])
        oos_idx = np.concatenate([np.arange(f * fold_size, (f+1) * fold_size) for f in oos_folds])

        is_idx  = is_idx[is_idx < T]
        oos_idx = oos_idx[oos_idx < T]

        if len(is_idx) == 0 or len(oos_idx) == 0:
            continue

        is_slice = values[is_idx]
        oos_slice = values[oos_idx]
        is_sharpes = np.nanmean(is_slice, axis=0) / (np.nanstd(is_slice, axis=0, ddof=1) + 1e-9)
        oos_sharpes = np.nanmean(oos_slice, axis=0) / (np.nanstd(oos_slice, axis=0, ddof=1) + 1e-9)

        best_is_idx = int(np.nanargmax(is_sharpes))
        rank_in_oos = int((oos_sharpes < oos_sharpes[best_is_idx]).sum())
        n_strats = int(len(oos_sharpes))

        # Relative OOS percentile of the IS-selected strategy.
        #
        # Higher Sharpe is better. `rank_in_oos` counts how many alternatives the
        # selected strategy beat OOS, so 0=worst and n-1=best.  CSCV/PBO asks how
        # often the IS winner lands below the OOS median.  We use a smoothed
        # percentile so edge cases (best/worst) contribute instead of being
        # silently dropped.
        w = (rank_in_oos + 1.0) / (n_strats + 1.0)
        logit_values.append(float(np.log(w / (1.0 - w))))

    if not logit_values:
        return {"pbo": 0.5, "pass": False, "unavailable": True, "note": "no_valid_splits"}

    pbo = float(np.mean(np.array(logit_values) <= 0))
    return {
        "pbo":  pbo,
        "pass": pbo < 0.5,
        "n_splits_used": len(logit_values),
        "n_splits_possible": n_splits_possible,
        "n_splits_sampled": len(split_combos),
    }


# ─────────────────────────────────────────────
# Monte Carlo block bootstrap
# ─────────────────────────────────────────────

def compute_mc_bootstrap(
    trade_returns: pd.Series | np.ndarray,
    n_bootstrap:   int   = 1000,
    block_size:    int   = 20,
    center_null:   bool  = True,
    pct_floor:     float = 5.0,
    alpha:         float = 0.05,
) -> dict:
    """
    Block bootstrap Monte Carlo on trade returns.
    center_null=True: bootstrap under the null (mean-shifted to 0) and gate
    by one-sided p-value.  center_null=False gates by the 5th percentile of
    the empirical bootstrap staying above zero.
    """
    returns = np.asarray(trade_returns, dtype=float)
    returns = returns[np.isfinite(returns)]

    if len(returns) < 10:
        return {"p5_pnl": 0.0, "pass": False, "note": "insufficient_trades"}

    observed_pnl = float(returns.sum())
    if center_null:
        returns = returns - returns.mean()

    n = len(returns)
    rng = np.random.default_rng(42)
    block_size = max(int(block_size), 1)
    n_blocks = int(np.ceil(n / block_size))
    starts = rng.integers(0, n, size=(int(n_bootstrap), n_blocks), dtype=np.int64)
    offsets = np.arange(block_size, dtype=np.int64)
    # Shape: bootstrap x blocks x block_size, then flatten to the original
    # trade count. This preserves circular block bootstrap semantics but avoids
    # thousands of tiny Python list/concatenate operations.
    sample_idx = (starts[:, :, None] + offsets[None, None, :]) % n
    sample_idx = sample_idx.reshape(int(n_bootstrap), n_blocks * block_size)[:, :n]
    cumulative_pnls = returns[sample_idx].sum(axis=1)
    p5  = float(np.percentile(cumulative_pnls, pct_floor))
    p50 = float(np.percentile(cumulative_pnls, 50))

    if center_null:
        p_value = float((cumulative_pnls >= observed_pnl).mean())
        passed = p_value <= float(alpha)
        test_type = "centered_null_p_value"
    else:
        p_value = float((cumulative_pnls <= 0).mean())
        passed = p5 > 0
        test_type = "empirical_p5_positive"

    return {
        "p5_pnl":  p5,
        "p50_pnl": p50,
        "p_value": p_value,
        "observed_pnl": observed_pnl,
        "test_type": test_type,
        "pass":    bool(passed),
    }


# ─────────────────────────────────────────────
# Deflated Sharpe Ratio (Bailey & de Prado 2014)
# ─────────────────────────────────────────────

def compute_dsr(
    observed_sharpe: float,
    n_trials:        int,
    n_obs:           int,
    skewness:        float = 0.0,
    kurtosis:        float = 3.0,
    sharpe_null:     float = 0.0,
) -> dict:
    """
    Deflated Sharpe Ratio (Bailey & de Prado 2014).

    Corrected implementation. The key insight is that E_max_SR is expressed in
    the same annualised units as observed_sharpe. The probability calculation
    is: DSR = Prob(SR* < observed_SR) under the multiple-testing correction.

    n_trials: total grid points tested (Bonferroni correction denominator).
    n_obs:    number of return observations (bars in the backtest).
    """
    if n_obs < 5 or n_trials < 1:
        return {"dsr": 0.0, "pass": False}

    # Expected maximum Sharpe under n_trials tests.
    # Bailey & de Prado assume independent trials. Grid combos are correlated
    # (neighbouring params produce similar strategies), so the effective number
    # of independent trials is much smaller than n_trials.
    # Conservative correction: effective_n = sqrt(n_trials), a common heuristic
    # that treats grid exploration as partially dependent.
    effective_n = max(int(np.sqrt(n_trials)), 2)
    gamma = 0.5772156649   # Euler-Mascheroni constant
    e_z = (
        (1 - gamma) * stats.norm.ppf(1 - 1.0 / effective_n) +
        gamma       * stats.norm.ppf(1 - 1.0 / (effective_n * np.e))
    ) if effective_n > 1 else 0.0

    # Non-normality adjustment on the observed SR
    # Converts observed SR to a t-stat-like value adjusted for skew/kurtosis
    excess_kurt = kurtosis - 3.0
    raw_variance_term = 1.0 - skewness * observed_sharpe + (excess_kurt / 4.0) * observed_sharpe ** 2
    # Bailey/de Prado's non-normality term can become zero or negative for
    # pathological skew/kurtosis estimates.  A near-zero clamp makes the z-stat
    # explode and can turn bad inputs into automatic passes, so use a modest
    # floor and expose the unclamped value for diagnostics.
    variance_term = max(raw_variance_term, 0.01)

    # SR* expressed as a z-score (per-observation units), then back to annualised
    # DSR = Φ( (SR_obs - SR_max*) * sqrt(n_obs - 1) / sigma_SR )
    # where sigma_SR = sqrt(variance_term) (Bailey & de Prado Eq. 7)
    sr_obs_adj   = (observed_sharpe - sharpe_null) / np.sqrt(variance_term)
    e_max_adj    = e_z / np.sqrt(variance_term)

    # The test statistic: how many standard errors is SR_obs above E_max?
    z = (sr_obs_adj - e_max_adj) * np.sqrt(n_obs - 1)
    dsr = float(stats.norm.cdf(z))

    return {
        "dsr":      dsr,
        "e_max_sr": float(e_z),
        "z_stat":   float(z),
        "variance_term": float(variance_term),
        "raw_variance_term": float(raw_variance_term),
        "pass":     dsr > 0.5,
    }


# ─────────────────────────────────────────────
# Neighbourhood degradation scoring
# ─────────────────────────────────────────────

def compute_neighborhood(
    candidate_params: dict,
    param_grid:       dict[str, list],
    results_df:       pd.DataFrame,
    param_names:      list[str],
    metric:           str   = "sharpe_ratio",
    max_degradation:  float = 0.30,
    lookup:           dict | None = None,
    ordered_values:   dict[str, list] | None = None,
    valid_params:     list[str] | None = None,
) -> dict:
    """
    For a candidate, look up ±1 grid-step neighbours and compute:
    - median neighbour score
    - degradation fraction relative to candidate score
    """
    valid_params = valid_params or [p for p in param_names if p in param_grid and p in results_df.columns]
    if lookup is None:
        lookup = {}
        for _, row in results_df.iterrows():
            key = tuple(row[p] for p in valid_params)
            lookup[key] = float(row[metric])
    ordered_values = ordered_values or {p: sorted(param_grid[p]) for p in valid_params}
    candidate_key = tuple(candidate_params.get(p) for p in valid_params)
    candidate_score = lookup.get(candidate_key, 0.0)

    neighbour_scores = []
    for dim_i, param in enumerate(valid_params):
        ordered = ordered_values[param]
        cur_val = candidate_params.get(param)
        if cur_val not in ordered:
            continue
        idx = ordered.index(cur_val)
        for delta in (-1, +1):
            ni = idx + delta
            if 0 <= ni < len(ordered):
                nb_key = list(candidate_key)
                nb_key[dim_i] = ordered[ni]
                nb_score = lookup.get(tuple(nb_key))
                if nb_score is not None:
                    neighbour_scores.append(nb_score)

    if not neighbour_scores:
        return {"degradation": 0.0, "median_neighbour": candidate_score, "pass": True}

    median_nb   = float(np.median(neighbour_scores))
    degradation = float((candidate_score - median_nb) / (abs(candidate_score) + 1e-9))

    return {
        "degradation":      degradation,
        "median_neighbour": median_nb,
        "n_neighbours":     len(neighbour_scores),
        "pass":             degradation < max_degradation,
    }


# ─────────────────────────────────────────────
# Composite scoring
# ─────────────────────────────────────────────

def composite_score(
    sharpe:          float,
    dsr:             float,
    plateau_w:       int,
    degradation:     float,
    pbo:             float,
    cycle_stability: float = 1.0,   # fraction of calendar windows where trades were profitable
    entropy_sharpe_score: float = 0.0,
    max_plateau:     int   = 10,
) -> float:
    """
    Single composite robustness score in [0, 1].
    Higher = more robust.

    cycle_stability: from Phase 2 calendar window scoring (0=fails all, 1=passes all).
    This is a scoring component, not a gate — a strategy that works in 3/4 windows
    still gets a positive score, it's just penalised vs one that works in 4/4.
    """
    s_norm  = min(max(sharpe / 3.0, 0.0), 1.0)
    d_norm  = min(max(dsr, 0.0), 1.0)
    pw_norm = min(plateau_w / max_plateau, 1.0)
    # Negative degradation means neighbors outperformed the center.  That is
    # useful evidence of a broad region, but it should not give extra credit
    # above 1.0 or the composite can over-reward non-local-optimum rows.
    nd_norm = min(max(0.0, 1.0 - degradation), 1.0)
    pbo_n   = max(0.0, 1.0 - pbo)
    cs_norm = max(0.0, min(cycle_stability, 1.0))

    ent_norm = max(0.0, min(float(entropy_sharpe_score), 1.0))

    # Weights: sharpe 15%, DSR 20%, plateau 15%, neighbourhood 15%, PBO 10%, cycle 20%, entropy 5%.  
    # These are subjective and can be tuned based on research priorities.
    # Weights: keep the old dimensions dominant, but reserve 5% for whether
    # the edge appears in structured, lower-entropy markets instead of random
    # markets.  This is a soft ranking signal, not a hard promotion gate.
    return float(
        s_norm  * 0.15 +
        d_norm  * 0.20 +
        pw_norm * 0.15 +
        nd_norm * 0.15 +
        pbo_n   * 0.10 +
        cs_norm * 0.20 +
        ent_norm * 0.05
    )


def compute_entropy_weighted_sharpe(
    trades_df: pd.DataFrame,
    labels,
    min_trades: int = 20,
) -> float:
    """
    Sharpe-like trade score weighted by structure at entry.

    Trades entered when shifted entropy is low get higher weight.  If this
    weighted score beats the raw candidate Sharpe, the edge likely comes from
    structured markets.  If it is much worse, the candidate may be benefiting
    from noisy/lucky high-entropy periods.
    """
    if trades_df is None or labels is None or len(trades_df) < int(min_trades):
        return float("nan")
    col_map = {str(c).lower().strip(): c for c in trades_df.columns}
    pnl_col = col_map.get("pnl after funding") or col_map.get("pnl") or col_map.get("return")
    entry_col = col_map.get("entry timestamp") or col_map.get("entry_timestamp") or col_map.get("entry index")
    if pnl_col is None or entry_col is None:
        return float("nan")
    try:
        entry_ts = pd.to_datetime(trades_df[entry_col], utc=True)
        pnl = pd.to_numeric(trades_df[pnl_col], errors="coerce").fillna(0.0)
        entropy = getattr(labels, "df", pd.DataFrame()).get("entropy_normalized")
        if entropy is None or len(entropy) == 0:
            return float("nan")
        weights = (1.0 - entropy.reindex(entry_ts, method="nearest").fillna(0.5)).clip(0.0, 1.0)
        weighted = pnl.to_numpy(dtype=float) * weights.to_numpy(dtype=float)
        std = float(np.nanstd(weighted, ddof=1))
        if std <= 0 or not np.isfinite(std):
            return 0.0
        return float(np.nanmean(weighted) / std * np.sqrt(252.0))
    except Exception:
        return float("nan")


def rank_candidates(
    results_df:    pd.DataFrame,
    param_grid:    dict[str, list],
    param_names:   list[str],
    topology,
    pipeline_cfg,
    metric: str = "sharpe_ratio",
) -> pd.DataFrame:
    """
    Phase 5: apply gates and rank candidates by composite score.
    Returns top-K enriched candidates DataFrame.

    FIX: PBO runs on best cluster only (not all 50 curves).
    FIX: equity curves retrieved from df.attrs["equity_store"].
    FIX: MC uses config values (n_bootstrap, block_size) not hardcoded constants.
    FIX: MC runs after selection (top-K), not on all passing combos.
    """
    df = results_df.copy()

    from pipeline.batch import apply_hard_gates
    df = apply_hard_gates(df, pipeline_cfg)
    if df.empty:
        return df

    n_trials = len(results_df)
    print(f"  Candidate gate rows after hard gates: {len(df)}/{len(results_df)}")

    # Retrieve equity store from batch results (FIX-5 in batch.py)
    equity_store = results_df.attrs.get("equity_store", {})
    trades_store = results_df.attrs.get("trades_store", {})
    regime_labels = results_df.attrs.get("regime_labels")
    if not equity_store:
        print("  Candidate selector note: equity_store unavailable; PBO will use the insufficient-curves fallback.")
    if not trades_store:
        print("  Candidate selector note: trades_store unavailable; MC will be skipped/fallback-pass.")

    valid_params = [p for p in param_names if p in param_grid and p in results_df.columns]
    ordered_values = {p: sorted(param_grid[p]) for p in valid_params}
    metric_lookup = {
        tuple(row[p] for p in valid_params): float(row.get(metric, 0.0))
        for _, row in results_df.iterrows()
    }

    # Plateau width lookup.  Key by (param_name, value) instead of raw value:
    # many grids reuse values such as 20 or 1.0 across unrelated parameters,
    # and a raw-value dictionary lets one parameter overwrite another.
    plateau_widths: dict[tuple[str, object], int] = {}
    if topology and topology.plateaus:
        for pl in topology.plateaus:
            for v in pl.param_values:
                plateau_widths[(pl.param_name, v)] = pl.width

    # First pass: compute neighborhood + DSR for all passing combos
    rows = []
    total_rows = len(df)
    for pos, (_, row) in enumerate(df.iterrows(), start=1):
        if total_rows >= 128 and (pos == 1 or pos % 128 == 0 or pos == total_rows):
            print(f"    candidate scoring {pos}/{total_rows} ({pos / total_rows:.0%})")
        params = {p: row[p] for p in param_names if p in df.columns}
        sharpe = float(row.get(metric, -99))

        # Neighborhood
        nb = compute_neighborhood(
            params, param_grid, results_df, param_names,
            metric=metric,
            max_degradation=pipeline_cfg.neighborhood_deg_max,
            lookup=metric_lookup,
            ordered_values=ordered_values,
            valid_params=valid_params,
        )

        # DSR — n_obs estimated from trade count × avg hold
        n_obs = max(
            int(row.get("n_trades", 0)) * max(int(row.get("avg_hold_bars", 1)), 1),
            50,
        )
        dsr_r = compute_dsr(sharpe, n_trials, n_obs)
        entropy_sharpe = compute_entropy_weighted_sharpe(trades_store.get(int(row.get("_combo_idx", -1))), regime_labels)
        entropy_sharpe_score = 0.0
        if np.isfinite(entropy_sharpe) and sharpe > 0:
            entropy_sharpe_score = float(np.clip(entropy_sharpe / max(sharpe, 1e-9), 0.0, 1.0))

        # Plateau width
        pw = max(
            (plateau_widths.get((p, row.get(p, 0)), 1) for p in param_names if p in df.columns),
            default=1,
        )

        rows.append({
            **params,
            metric:           sharpe,
            "total_return":   float(row.get("total_return", -99)),
            "max_drawdown":   float(row.get("max_drawdown", -99)),
            "calmar_ratio":   float(row.get("calmar_ratio", 0)),
            "n_trades":       int(row.get("n_trades", 0)),
            "win_rate":       float(row.get("win_rate", 0)),
            "profit_factor":  float(row.get("profit_factor", 0)),
            "avg_hold_bars":  float(row.get("avg_hold_bars", 0)),
            "dsr":            dsr_r.get("dsr", 0.0),
            "dsr_pass":       dsr_r.get("pass", False),
            "degradation":    nb.get("degradation", 0.0),
            "nb_pass":        nb.get("pass", True),
            "plateau_width":  pw,
            "cycle_stability": float(row.get("cycle_stability", 1.0)),
            "entropy_weighted_sharpe": float(entropy_sharpe) if np.isfinite(entropy_sharpe) else np.nan,
            "entropy_sharpe_score": entropy_sharpe_score,
            "_combo_idx":     int(row.get("_combo_idx", -1)),
        })

    if not rows:
        return pd.DataFrame()

    pre_df = pd.DataFrame(rows)
    print(f"  Candidate selector enriched rows: {len(pre_df)}")

    # Gate: neighborhood + DSR
    pre_df = pre_df[pre_df["nb_pass"]].copy()
    if pre_df.empty:
        return pre_df
    print(f"  Candidate rows after neighborhood gate: {len(pre_df)}")

    # PBO on best cluster (top ~10 by Sharpe) — not all 50 (FIX)
    best_cluster = pre_df.nlargest(min(10, len(pre_df)), metric)
    best_idxs = best_cluster["_combo_idx"].tolist()
    best_equities = [equity_store[i] for i in best_idxs if i in equity_store]
    print(f"  PBO CSCV on {len(best_equities)} top equity curve(s)…")
    pbo_result = compute_pbo_cscv(
        best_equities,
        n_splits=min(16, max(4, len(best_equities) * 2)),
        max_combinations=512,
    )
    print(
        f"  PBO={pbo_result.get('pbo', 0.5):.3f} pass={pbo_result.get('pass', True)} "
        f"splits={pbo_result.get('n_splits_used', 0)}/{pbo_result.get('n_splits_possible', pbo_result.get('n_splits_used', 0))}"
    )

    # MC on top-K only — after preliminary ranking (FIX: use config values)
    pre_df["pbo"]       = pbo_result.get("pbo", 0.5)
    pre_df["pbo_pass"]  = pbo_result.get("pass", True)
    pre_df["mc_p5_pnl"] = 0.0
    pre_df["mc_pass"]   = True

    # Preliminary sort by DSR × plateau × neighborhood to find top-K candidates for MC
    pre_df["_prescore"] = (
        pre_df["dsr"].clip(0, 1) * 0.4 +
        (pre_df["plateau_width"] / max(pre_df["plateau_width"].max(), 1)) * 0.3 +
        (1 - pre_df["degradation"].clip(0, 1)) * 0.3
    )
    top_k_pre = pre_df.nlargest(pipeline_cfg.top_k * 2, "_prescore")

    # Run MC only on these candidates
    print(f"  Monte Carlo bootstrap on {len(top_k_pre)} preliminary candidate(s)…")
    for mc_pos, idx in enumerate(top_k_pre.index, start=1):
        if len(top_k_pre) >= 6 and (mc_pos == 1 or mc_pos == len(top_k_pre)):
            print(f"    MC candidate {mc_pos}/{len(top_k_pre)}")
        combo_idx = int(pre_df.loc[idx, "_combo_idx"])
        trades_df = trades_store.get(combo_idx)
        if trades_df is not None and len(trades_df) > 10:
            trade_rets = trades_df["PnL"].values / 10_000.0   # normalise by init_cash
            mc_r = compute_mc_bootstrap(
                trade_rets,
                n_bootstrap = pipeline_cfg.mc_n_bootstrap,   # FIX: use config
                block_size  = pipeline_cfg.mc_block_size,     # FIX: use config
            )
            pre_df.loc[idx, "mc_p5_pnl"] = mc_r.get("p5_pnl", 0.0)
            pre_df.loc[idx, "mc_pass"]   = mc_r.get("pass", True)

    # Final gate filter
    gate_mask = (
        pre_df["pbo_pass"] &
        pre_df["nb_pass"]  &
        pre_df["mc_pass"]
    )
    out = pre_df[gate_mask].copy()
    if out.empty:
        # Do not hide strong research rows just because an inline diagnostic
        # gate failed.  Phase 6 PROVE is the place where WFO/slippage/multi-slice
        # evidence should explicitly accept or reject a near-miss candidate.
        # Returning the best near-misses keeps reports actionable and avoids the
        # misleading "No candidates" outcome on high-Sharpe robust grids.
        out = top_k_pre.copy()
        out["selection_status"] = "near_miss_failed_inline_gate"
    else:
        out["selection_status"] = "passed_inline_gate"

    # Composite score
    max_pw = max(out["plateau_width"].max(), 1)
    out["composite_score"] = out.apply(
        lambda r: composite_score(
            sharpe          = r[metric],
            dsr             = r["dsr"],
            plateau_w       = r["plateau_width"],
            degradation     = r["degradation"],
            pbo             = r["pbo"],
            cycle_stability = float(r.get("cycle_stability", 1.0)),
            entropy_sharpe_score = float(r.get("entropy_sharpe_score", 0.0)),
            max_plateau     = max_pw,
        ),
        axis=1,
    )

    return out.sort_values("composite_score", ascending=False).head(pipeline_cfg.top_k)



"""Grid and plateau diagnostics for auto-analysis."""
from __future__ import annotations

import numpy as np
import pandas as pd

from ..stats import distribution_stats
from .gates import GATE_DEFAULTS


def _safe_float(value) -> float | None:
    try:
        value = float(value)
        return value if pd.notna(value) and value not in (float("inf"), -float("inf")) else None
    except Exception:
        return None


def _local_neighborhood_metrics(row: pd.Series, df: pd.DataFrame, param_names: list[str], metric: str = "sharpe_ratio") -> dict:
    """Measure the full +/-1 grid-index neighborhood around one combo."""
    if df.empty or metric not in df.columns:
        return {"available": False, "reason": f"{metric} was unavailable."}
    valid_params = [p for p in param_names if p in df.columns and p in row.index]
    if not valid_params:
        return {"available": False, "reason": "No grid params were available."}
    allowed: dict[str, set] = {}
    boundary_params = []
    for param in valid_params:
        values = sorted(df[param].dropna().unique().tolist())
        value = row.get(param)
        if value not in values:
            continue
        pos = values.index(value)
        if pos == 0:
            boundary_params.append(f"{param}_lower")
        if pos == len(values) - 1:
            boundary_params.append(f"{param}_upper")
        allowed[param] = set(values[max(0, pos - 1): min(len(values), pos + 2)])
    mask = pd.Series(True, index=df.index)
    for param, values in allowed.items():
        mask &= df[param].isin(values)
    neighborhood = df.loc[mask].copy()
    center_idx = row.name
    if center_idx in neighborhood.index:
        neighborhood = neighborhood.drop(index=center_idx)
    scores = pd.to_numeric(neighborhood.get(metric, pd.Series(dtype=float)), errors="coerce").dropna()
    best_score = _safe_float(row.get(metric)) or 0.0
    pf = pd.to_numeric(neighborhood.get("profit_factor", pd.Series(dtype=float)), errors="coerce")
    dd = pd.to_numeric(neighborhood.get("max_drawdown", pd.Series(dtype=float)), errors="coerce")
    trades = pd.to_numeric(neighborhood.get("n_trades", pd.Series(dtype=float)), errors="coerce")
    pass_mask = (
        (scores >= GATE_DEFAULTS["min_score"])
        & (pf.reindex(scores.index) >= GATE_DEFAULTS["min_profit_factor"]).fillna(False)
        & (dd.reindex(scores.index) >= GATE_DEFAULTS["max_drawdown_floor"]).fillna(False)
        & (trades.reindex(scores.index) >= GATE_DEFAULTS["min_trades"]).fillna(False)
    )
    score_distribution = distribution_stats(scores)
    median_score = score_distribution.get("median")
    p25_score = score_distribution.get("p25")
    pass_rate = float(pass_mask.sum() / max(len(scores), 1)) if len(scores) else 0.0
    ratio = (best_score / median_score) if median_score and median_score > 0 else None
    catastrophic = int((scores < 0).sum()) if len(scores) else 0
    if not len(scores):
        classification = "no_neighbors"
    elif best_score < GATE_DEFAULTS["min_score"]:
        # A low absolute best score is not a spike; it is simply weak edge.
        classification = "weak_no_edge"
    elif (
        pass_rate < 0.30
        or (median_score is not None and median_score < best_score * 0.50)
        or (ratio is not None and ratio > 2.0 and median_score is not None and abs(median_score) > 0.05)
    ):
        classification = "spike"
    elif boundary_params and pass_rate >= 0.50 and median_score is not None and median_score >= best_score * 0.60:
        classification = "boundary_incomplete_plateau"
    elif pass_rate >= 0.60 and median_score is not None and median_score >= best_score * 0.70 and catastrophic == 0:
        classification = "acceptable_plateau"
    else:
        classification = "mixed_or_fragile"
    return {
        "available": True,
        "metric_name": metric,
        "distance_definition": "all combos within +/-1 grid index on every parameter, excluding the center combo",
        "neighbor_count": int(len(scores)),
        "score_distribution": score_distribution,
        "pass_rate": pass_rate,
        "best_score": best_score,
        "best_minus_neighbor_median": (best_score - median_score) if median_score is not None else None,
        "best_to_neighbor_median_ratio": ratio,
        "p25_neighborhood_score": p25_score,
        "median_neighborhood_score": median_score,
        "catastrophic_neighbor_count": catastrophic,
        "boundary_params": boundary_params,
        "classification": classification,
        "interpretation": "Local neighborhood metrics test whether the top combo survives one grid step in every parameter.",
        "suggestion": "Promote only acceptable plateaus; expand boundary-incomplete plateaus before treating the best combo as final.",
    }


def _contiguous_plateaus(values: list, scores: list[float], tolerance_pct: float = 0.15) -> list[dict]:
    """Find contiguous marginal-score regions near the best score."""
    if not values or not scores:
        return []
    pairs = [(v, float(s)) for v, s in zip(values, scores) if pd.notna(s) and np.isfinite(float(s))]
    if not pairs:
        return []
    values = [v for v, _ in pairs]
    scores = [s for _, s in pairs]
    best = max(float(s) for s in scores)
    if best <= 0:
        return []
    threshold = best * (1.0 - tolerance_pct)
    plateaus = []
    start = None
    for i, score in enumerate(scores):
        if float(score) >= threshold and start is None:
            start = i
        elif float(score) < threshold and start is not None:
            plateaus.append((start, i - 1))
            start = None
    if start is not None:
        plateaus.append((start, len(scores) - 1))

    rows = []
    for s, e in plateaus:
        plateau_scores = [float(scores[i]) for i in range(s, e + 1)]
        center_i = s + int(np.argmax(plateau_scores))
        center_score = float(scores[center_i])
        min_plateau = float(np.min(plateau_scores))
        median_plateau = float(np.median(plateau_scores))
        std_plateau = float(np.std(plateau_scores))
        valley_depth = float((center_score - min_plateau) / max(abs(center_score), 1e-9))
        has_internal_valley = bool(len(plateau_scores) >= 3 and valley_depth > 0.25)
        if has_internal_valley:
            plateau_type = "valley_or_two_peaks"
        elif std_plateau < 0.15:
            plateau_type = "flat"
        elif plateau_scores == sorted(plateau_scores) or plateau_scores == sorted(plateau_scores, reverse=True):
            plateau_type = "sloped"
        else:
            plateau_type = "ridge"
        neighbor_scores = []
        if s - 1 >= 0:
            neighbor_scores.append(float(scores[s - 1]))
        if e + 1 < len(scores):
            neighbor_scores.append(float(scores[e + 1]))
        neighbor_avg = float(np.mean(neighbor_scores)) if neighbor_scores else float(np.mean(plateau_scores))
        consistency = neighbor_avg / center_score if center_score > 0 else 0.0
        width = e - s + 1
        neighbor_degradation = float((center_score - neighbor_avg) / max(abs(center_score), 1e-9))
        rows.append({
            "center": values[center_i],
            "bounds": [values[s], values[e]],
            "width_in_steps": int(width),
            "width_pct_of_range": float(width / max(len(values), 1)),
            "avg_sharpe_in_plateau": float(np.mean(plateau_scores)),
            "median_sharpe": median_plateau,
            "min_sharpe": min_plateau,
            "sharpe_std_in_plateau": std_plateau,
            "all_plateau_sharpes": plateau_scores,
            "plateau_type": plateau_type,
            "has_internal_valley": has_internal_valley,
            "valley_depth": valley_depth,
            "neighbor_avg": neighbor_avg,
            "neighbor_degradation": neighbor_degradation,
            "consistency_ratio": consistency,
            "is_robust": bool(width >= 3 and consistency >= 0.85 and not has_internal_valley),
            "warning": None if width >= 3 else "Narrow plateau; add intermediate values before trusting this parameter.",
        })
    return rows



"""
param_analysis.py — marginal parameter diagnostics for grid results.

The goal is not to overfit the next grid automatically.  The goal is to show
clear evidence: boundary pressure, flat parameters, noisy parameters, and the
direction where the current grid appears to improve.
"""
from __future__ import annotations

import math
import pandas as pd

from .stats import distribution_stats


METRIC_COLUMNS = {
    "sharpe_ratio", "total_return", "max_drawdown", "calmar_ratio",
    "n_trades", "win_rate", "avg_hold_bars", "profit_factor",
    "composite_score", "pbo", "dsr", "degradation", "cycle_stability",
}
META_COLUMNS = {"exit_mode", "exit_label", "_combo_idx", "_error"}


def infer_grid_params(results_df: pd.DataFrame) -> list[str]:
    """Infer tunable columns by excluding metrics, metadata, and private fields."""
    params: list[str] = []
    for col in results_df.columns:
        if col in METRIC_COLUMNS or col in META_COLUMNS or col.startswith("_"):
            continue
        if results_df[col].nunique(dropna=False) >= 1:
            params.append(col)
    return params


def analyze_params(
    results_df: pd.DataFrame,
    param_names: list[str],
    metric: str = "sharpe_ratio",
) -> dict:
    """Compute per-param marginal scores and beginner-readable classifications."""
    if results_df.empty or metric not in results_df.columns:
        return {}

    best_idx = results_df[metric].idxmax()
    best_row = results_df.loc[best_idx]
    analysis: dict[str, dict] = {}

    for param in param_names:
        if param not in results_df.columns:
            continue
        # Group by one parameter while marginalizing over all other parameters.
        # Median and quartiles matter more than mean alone because one lucky
        # combo can pull the mean up and make a weak parameter value look good.
        grouped = results_df.groupby(param, dropna=False).agg(
            mean_score=(metric, "mean"),
            median_score=(metric, "median"),
            p25_score=(metric, lambda s: s.quantile(0.25)),
            p75_score=(metric, lambda s: s.quantile(0.75)),
            p90_score=(metric, lambda s: s.quantile(0.90)),
            max_score=(metric, "max"),
            min_score=(metric, "min"),
            score_std=(metric, "std"),
            n_positive=("total_return", lambda s: int((pd.to_numeric(s, errors="coerce") > 0).sum())) if "total_return" in results_df.columns else (metric, lambda s: int((pd.to_numeric(s, errors="coerce") > 0).sum())),
            median_trades=("n_trades", "median") if "n_trades" in results_df.columns else (metric, "count"),
            median_win_rate=("win_rate", "median") if "win_rate" in results_df.columns else (metric, "count"),
            median_profit_factor=("profit_factor", "median") if "profit_factor" in results_df.columns else (metric, "count"),
            count=(metric, "count"),
        ).reset_index().sort_values(param)
        values = grouped[param].tolist()
        scores = grouped["mean_score"].astype(float).tolist()
        median_scores = grouped["median_score"].astype(float).tolist()
        p25_scores = grouped["p25_score"].astype(float).tolist()
        p75_scores = grouped["p75_score"].astype(float).tolist()
        max_scores = grouped["max_score"].astype(float).tolist()
        min_scores = grouped["min_score"].astype(float).tolist()
        std_scores = grouped["score_std"].fillna(0.0).astype(float).tolist()
        per_value_table = _per_value_table(grouped, results_df, param, metric)
        best_value = best_row.get(param)
        best_at_lower = bool(values and best_value == values[0])
        best_at_upper = bool(values and best_value == values[-1])
        spread = float(max(scores) - min(scores)) if scores else 0.0
        boundary_pressure = "upper" if best_at_upper else "lower" if best_at_lower else "interior"
        direction = "flat"
        if len(values) >= 2 and scores[-1] > scores[0]:
            direction = "higher_values_help"
        elif len(values) >= 2 and scores[-1] < scores[0]:
            direction = "lower_values_help"

        if best_at_upper:
            classification = "upper_boundary_pressure"
        elif best_at_lower:
            classification = "lower_boundary_pressure"
        elif spread < 0.10:
            classification = "flat_or_low_impact"
        else:
            classification = "interior_or_mixed"

        best_pos = values.index(best_value) if best_value in values else int(pd.Series(scores).idxmax())
        relative_spread = spread / max(abs(float(pd.to_numeric(results_df[metric], errors="coerce").mean() or 0.0)), 1e-9)
        spearman_param_score_corr = _spearman(results_df[param], results_df[metric])
        spearman_param_trade_count_corr = _spearman(results_df[param], results_df["n_trades"]) if "n_trades" in results_df.columns else None
        monotonic_score = _monotonic_label(median_scores)
        score_consistency_by_value = {
            str(row["param_value"]): row.get("score_consistency")
            for row in per_value_table
        }
        score_stability_at_value = {
            str(row["param_value"]): row.get("score_stability")
            for row in per_value_table
        }
        score_per_trade_by_value = {
            str(row["param_value"]): row.get("score_per_trade")
            for row in per_value_table
        }
        quartile_table = [
            {
                "param_value": row["param_value"],
                "q1_score": row.get("p25_score"),
                "q2_score": row.get("median_score"),
                "q3_score": row.get("p75_score"),
                "q4_score": row.get("max_score"),
            }
            for row in per_value_table
        ]
        neighbour_scores = [
            scores[i] for i in (best_pos - 1, best_pos + 1)
            if 0 <= i < len(scores)
        ]
        neighbour_avg = float(sum(neighbour_scores) / len(neighbour_scores)) if neighbour_scores else None
        best_score = float(scores[best_pos]) if scores else 0.0
        dropoff = None
        if neighbour_avg is not None and abs(best_score) > 1e-12:
            dropoff = float((neighbour_avg - best_score) / abs(best_score))

        plateau_width_1step = _plateau_width_around_best(scores, best_pos, tolerance_pct=0.10)
        plateau_width_2step = _plateau_width_around_best(scores, best_pos, tolerance_pct=0.20)
        median_at_best = float(median_scores[best_pos]) if median_scores else 0.0
        stability_score = None
        if median_at_best and math.isfinite(median_at_best):
            # Normalize by a real floor so near-zero edges do not explode into
            # nonsense stability scores such as 50x. Values above 1 are all
            # "fragile enough" for decision purposes, so clamp there.
            stability_score = min(
                abs(float(p75_scores[best_pos] - p25_scores[best_pos]) / (abs(median_at_best) + 0.5)),
                1.0,
            )

        recommendation = _recommend_param(
            param=param,
            values=values,
            scores=scores,
            best_pos=best_pos,
            classification=classification,
            direction=direction,
            plateau_width=plateau_width_1step,
        )

        analysis[param] = {
            "values": values,
            "score_metrics": {
                "mean_sharpe": scores,
                "median_sharpe": median_scores,
                "p25_sharpe": p25_scores,
                "p75_sharpe": p75_scores,
                "max_sharpe": max_scores,
                "min_sharpe": min_scores,
                "sharpe_std": std_scores,
                "count_valid": grouped["count"].astype(int).tolist(),
            },
            "robustness_metrics": {
                "plateau_width_1step": int(plateau_width_1step),
                "plateau_width_2step": int(plateau_width_2step),
                "dropoff_after_best": dropoff,
                "boundary_pressure": boundary_pressure if boundary_pressure != "interior" else "none",
                "stability_score": stability_score,
            },
            "trade_quality": {
                "median_trades": grouped["median_trades"].astype(float).tolist(),
                "median_win_rate": grouped["median_win_rate"].astype(float).tolist(),
                "median_profit_factor": grouped["median_profit_factor"].astype(float).tolist(),
            },
            "metric_name": metric,
            "per_value_table": per_value_table,
            "unit_convention": "pct_profitable is a fractional percentage: 0.60 means 60%. score_signal_to_noise is mean_score / std_score.",
            "mean_scores": scores,
            "best_value": best_value,
            "best_at_lower_boundary": best_at_lower,
            "best_at_upper_boundary": best_at_upper,
            "score_spread": spread,
            "relative_spread": relative_spread,
            "monotonic_score": monotonic_score,
            "spearman_param_score_corr": spearman_param_score_corr,
            "spearman_param_trade_count_corr": spearman_param_trade_count_corr,
            "score_consistency_by_value": score_consistency_by_value,
            "score_stability_at_value": score_stability_at_value,
            "score_per_trade_by_value": score_per_trade_by_value,
            "quartile_table": quartile_table,
            "direction_hint": direction,
            "classification": classification,
            "recommendation": recommendation,
            "table": grouped.to_dict(orient="records"),
        }
    return analysis


def _classify_value(row: pd.Series) -> tuple[str, str, str]:
    """Classify one parameter value using distribution shape, not max alone."""
    mean_score = float(row.get("mean_score", 0.0) or 0.0)
    median_score = float(row.get("median_score", 0.0) or 0.0)
    p25_score = float(row.get("p25_score", 0.0) or 0.0)
    p75_score = float(row.get("p75_score", 0.0) or 0.0)
    max_score = float(row.get("max_score", 0.0) or 0.0)
    std_score = float(row.get("score_std", 0.0) or 0.0)
    count = max(int(row.get("count", 0) or 0), 1)
    pct_profitable = float(row.get("n_positive", 0) or 0) / count
    iqr = p75_score - p25_score
    cv = std_score / max(abs(mean_score), 1e-9)
    if median_score <= 0:
        classification = "dead"
        interpretation = "Median score is non-positive, so most combinations around this value have no reliable edge."
        suggestion = "Do not expand around this value unless another diagnostic proves an interaction effect."
    elif max_score > max(median_score * 2.0, median_score + 0.50) and pct_profitable < 0.50:
        classification = "spike"
        interpretation = "The max score is much better than the median, so one lucky combo may be driving the value."
        suggestion = "Treat as overfit until fixed-slice and neighbor stability confirm it."
    elif cv > 1.0 or iqr > max(abs(median_score), 1e-9):
        classification = "fragile"
        interpretation = "The value has high variability across other parameters."
        suggestion = "Add midpoints or avoid this value unless it is required by a wider 2D ridge."
    elif pct_profitable >= 0.60 and cv < 0.50:
        classification = "robust"
        interpretation = "Most combinations are profitable and score dispersion is controlled."
        suggestion = "Keep this value and refine nearby values in the next sweep."
    else:
        classification = "mixed"
        interpretation = "The value is neither clearly robust nor clearly dead."
        suggestion = "Use slice profiles and neighbor stability before deciding whether to keep it."
    return classification, interpretation, suggestion


def _metric_distribution(df: pd.DataFrame, column: str) -> dict:
    """Return canonical stats for a metric column, or an empty stats block."""
    if column not in df.columns:
        return distribution_stats([])
    return distribution_stats(df[column])


def _pass_rates(df: pd.DataFrame) -> dict:
    """Return the pass-rate diagnostics used to judge one param value."""
    n = max(int(len(df)), 1)
    sharpe = pd.to_numeric(df.get("sharpe_ratio", pd.Series(dtype=float)), errors="coerce")
    total_return = pd.to_numeric(df.get("total_return", pd.Series(dtype=float)), errors="coerce")
    pf = pd.to_numeric(df.get("profit_factor", pd.Series(dtype=float)), errors="coerce")
    dd = pd.to_numeric(df.get("max_drawdown", pd.Series(dtype=float)), errors="coerce")
    trades = pd.to_numeric(df.get("n_trades", pd.Series(dtype=float)), errors="coerce")
    gates = (
        (sharpe >= 0.75).fillna(False)
        & (pf >= 1.20).fillna(False)
        & (dd >= -0.35).fillna(False)
        & (trades >= 80).fillna(False)
    )
    return {
        "profitable_pct": float((total_return > 0).fillna(False).sum() / n),
        "sharpe_above_0_pct": float((sharpe > 0).fillna(False).sum() / n),
        "sharpe_above_0_75_pct": float((sharpe >= 0.75).fillna(False).sum() / n),
        "pf_above_1_2_pct": float((pf >= 1.20).fillna(False).sum() / n),
        "dd_below_30_pct": float((dd >= -0.30).fillna(False).sum() / n),
        "min_trades_pass_pct": float((trades >= 80).fillna(False).sum() / n),
        "all_gates_pass_pct": float(gates.sum() / n),
    }


def _spearman(left: pd.Series, right: pd.Series) -> float | None:
    """Return Spearman correlation for uneven grid values, or None if invalid."""
    try:
        frame = pd.DataFrame({
            "left": pd.to_numeric(left, errors="coerce"),
            "right": pd.to_numeric(right, errors="coerce"),
        }).dropna()
        if len(frame) < 3 or frame["left"].nunique() < 2 or frame["right"].nunique() < 2:
            return None
        value = frame["left"].corr(frame["right"], method="spearman")
        return float(value) if pd.notna(value) else None
    except Exception:
        return None


def _monotonic_label(values: list[float]) -> str:
    """Classify median-score direction across tested parameter values."""
    if len(values) < 2:
        return "insufficient_values"
    if all(values[i] <= values[i + 1] for i in range(len(values) - 1)):
        return "increasing"
    if all(values[i] >= values[i + 1] for i in range(len(values) - 1)):
        return "decreasing"
    best_idx = int(pd.Series(values).idxmax())
    if 0 < best_idx < len(values) - 1:
        return "interior_peak"
    return "non_monotonic"


def _per_value_table(grouped: pd.DataFrame, results_df: pd.DataFrame, param: str, metric: str) -> list[dict]:
    """Build the expert-requested per-value table while preserving old arrays."""
    rows: list[dict] = []
    for _, row in grouped.iterrows():
        raw = results_df[results_df[param].eq(row.iloc[0])]
        count = max(int(row.get("count", 0) or 0), 1)
        mean_score = float(row.get("mean_score", 0.0) or 0.0)
        std_score = float(row.get("score_std", 0.0) or 0.0)
        score_range = float((row.get("max_score", 0.0) or 0.0) - (row.get("min_score", 0.0) or 0.0))
        cv = std_score / max(abs(mean_score), 1e-9)
        snr = mean_score / std_score if std_score > 1e-12 else None
        median_score = float(row.get("median_score", 0.0) or 0.0)
        max_score = float(row.get("max_score", 0.0) or 0.0)
        score_consistency = median_score / max_score if abs(max_score) > 1e-9 else None
        mean_trades = float(pd.to_numeric(raw.get("n_trades", pd.Series(dtype=float)), errors="coerce").mean()) if "n_trades" in raw else None
        mean_drawdown = float(pd.to_numeric(raw.get("max_drawdown", pd.Series(dtype=float)), errors="coerce").mean()) if "max_drawdown" in raw else None
        mean_profit_factor = float(pd.to_numeric(raw.get("profit_factor", pd.Series(dtype=float)), errors="coerce").mean()) if "profit_factor" in raw else None
        score_per_trade = mean_score / mean_trades if mean_trades and mean_trades > 0 else None
        classification, interpretation, suggestion = _classify_value(row)
        score_distribution = _metric_distribution(raw, metric)
        rows.append({
            "value": row.iloc[0],
            "param_value": row.iloc[0],
            "metric_name": metric,
            "score_distribution": score_distribution,
            "return_distribution": _metric_distribution(raw, "total_return"),
            "drawdown_distribution": _metric_distribution(raw, "max_drawdown"),
            "profit_factor_distribution": _metric_distribution(raw, "profit_factor"),
            "trade_count_distribution": _metric_distribution(raw, "n_trades"),
            "pass_rates": _pass_rates(raw),
            "mean_score": mean_score,
            "median_score": median_score,
            "std_score": std_score,
            "min_score": float(row.get("min_score", 0.0) or 0.0),
            "max_score": max_score,
            "p25_score": float(row.get("p25_score", 0.0) or 0.0),
            "p75_score": float(row.get("p75_score", 0.0) or 0.0),
            "p90_score": float(row.get("p90_score", 0.0) or 0.0),
            "score_range": score_range,
            "cv": cv,
            "score_signal_to_noise": snr,
            "score_consistency": score_consistency,
            "score_stability": cv,
            "mean_trades": mean_trades,
            "mean_drawdown": mean_drawdown,
            "mean_profit_factor": mean_profit_factor,
            "score_per_trade": score_per_trade,
            "pct_profitable": float(row.get("n_positive", 0) or 0) / count,
            "n_positive": int(row.get("n_positive", 0) or 0),
            "median_trades": float(row.get("median_trades", 0.0) or 0.0),
            "count": int(row.get("count", 0) or 0),
            "classification": classification,
            "interpretation": interpretation,
            "suggestion": suggestion,
        })
    return rows


def _plateau_width_around_best(scores: list[float], best_pos: int, tolerance_pct: float) -> int:
    """
    Count contiguous values around the best point that remain near the best.

    Width is measured in grid steps. A width of 1 means a single-point spike.
    A width of 3 or more means the parameter can move one step either way and
    still stay close to the best value.
    """
    if not scores or best_pos < 0 or best_pos >= len(scores):
        return 0
    best_score = float(scores[best_pos])
    if best_score <= 0:
        return 1
    threshold = best_score * (1.0 - tolerance_pct)
    width = 1
    i = best_pos - 1
    while i >= 0 and float(scores[i]) >= threshold:
        width += 1
        i -= 1
    i = best_pos + 1
    while i < len(scores) and float(scores[i]) >= threshold:
        width += 1
        i += 1
    return width


def _recommend_param(
    param: str,
    values: list,
    scores: list[float],
    best_pos: int,
    classification: str,
    direction: str,
    plateau_width: int,
) -> str:
    """Create a concrete next-grid recommendation for one parameter."""
    if not values:
        return "No values were available for this parameter."
    best_value = values[best_pos]
    if classification == "upper_boundary_pressure":
        return f"Best {param} is at the upper boundary ({best_value}); test higher values while watching trade count decay."
    if classification == "lower_boundary_pressure":
        return f"Best {param} is at the lower boundary ({best_value}); test lower values if strategy logic permits it."
    if plateau_width >= 3:
        return f"{param} has a useful plateau; refine around {best_value} instead of expanding the full range."
    if direction == "higher_values_help":
        return f"{param} improves toward higher values but plateau is narrow; add intermediate and slightly higher values."
    if direction == "lower_values_help":
        return f"{param} improves toward lower values but plateau is narrow; add intermediate and slightly lower values."
    return f"{param} looks mixed or flat; keep current values unless another diagnostic shows interaction effects."

```