#!/usr/bin/env python3
"""Make the README figures from the scenario CSVs (run `make -C tests csv` first).
   python3 tools/make_plots.py      -> docs/images/plot_*.png"""
import csv, pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA, OUT = ROOT / "docs/data", ROOT / "docs/images"
S1, S2, S3 = "#2a78d6", "#eb6834", "#1baf7a"          # categorical slots 1-3
INK, INK2, GRID, SURF = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
CRIT = "#c62828"                                        # status: critical (limits)

plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF, "axes.edgecolor": GRID,
    "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2, "text.color": INK,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "font.size": 10,
    "lines.linewidth": 2, "legend.frameon": False, "axes.titleweight": "bold",
    "axes.titlesize": 11, "axes.titlelocation": "left"})

def load(name):
    with open(DATA / f"{name}.csv") as f:
        rows = list(csv.DictReader(f))
    col = lambda k: [float(r[k]) for r in rows]
    return rows, col

def first(rows, pred):
    for r in rows:
        if pred(r): return float(r["t_s"])

def limit(ax, y, text):
    ax.axhline(y, color=CRIT, lw=1.2, ls="--")
    ax.text(ax.get_xlim()[1], y, f" {text}", color=CRIT, va="center", ha="left", fontsize=9)

# ---- 1. a normal fill: pressure, tank temperature, state of charge ----
rows, c = load("normal_fill")
t = c("t_s")
fig, ax = plt.subplots(3, 1, figsize=(8, 7.2), sharex=True)
ramp = first(rows, lambda r: r["phase"] == "RAMP")
tgt = [p if p > 0 else None for p in c("p_target_mpa")]
ax[0].plot(t, c("p_true_mpa"), color=S1, label="tank pressure (model)")
ax[0].plot(t, tgt, color=S2, ls="--", lw=1.5, label="PI ramp target, 1 MPa/s")
ax[0].axvspan(0, ramp, color=GRID, alpha=0.6, lw=0)
ax[0].text(ramp / 2, ax[0].get_ylim()[1] * 0.92, "leak\ncheck", ha="center", va="top", fontsize=8, color=INK2)
ax[0].set_ylabel("MPa"); ax[0].set_title("Pressure follows the ramp target"); ax[0].legend(loc="upper left", bbox_to_anchor=(0.12, 1))
ax[1].plot(t, c("tank_c"), color=S1)
ax[1].set_ylim(20, 95); ax[1].set_xlim(0, t[-1]); limit(ax[1], 85, "85 °C tank limit")
ax[1].set_ylabel("°C"); ax[1].set_title("Tank gas temperature (pre-cooled gas at −40 °C)")
ax[2].plot(t, c("soc_pct"), color=S1); ax[2].set_ylim(0, 105); limit(ax[2], 97, "97 % end of fill")
ax[2].set_ylabel("%"); ax[2].set_title("State of charge"); ax[2].set_xlabel("time since START [s]")
fig.tight_layout(); fig.savefig(OUT / "plot_normal_fill.png", dpi=130); plt.close(fig)

# ---- 2. pre-cooler failure vs normal ----
rn, cn = load("normal_fill"); rp, cp = load("precool_failure")
fig, ax = plt.subplots(2, 1, figsize=(8, 5.6), sharex=True)
ax[0].plot(cn("t_s"), cn("tt1_c"), color=S1, label="normal")
ax[0].plot(cp("t_s"), cp("tt1_c"), color=S2, label="pre-cooler fails at 5 s")
ax[0].set_ylabel("°C"); ax[0].set_title("TT1: gas temperature from the pre-cooler"); ax[0].legend(loc="upper right")
ax[1].plot(cn("t_s"), cn("tank_c"), color=S1, label="normal")
ax[1].plot(cp("t_s"), cp("tank_c"), color=S2, label="pre-cooler fails")
ax[1].set_xlim(0, cn("t_s")[-1]); ax[1].set_ylim(20, 95); limit(ax[1], 85, "85 °C → STOP")
trip = first(rp, lambda r: r["state"] != "FILLING")
soc = next(float(r["soc_pct"]) for r in rp if float(r["t_s"]) >= trip)
ax[1].annotate(f"TANK_OVERTEMP\nstopped at {trip:.0f} s, SOC {soc:.0f} %", (trip, 85), (trip + 3, 58),
               arrowprops=dict(arrowstyle="->", color=INK2), fontsize=9, color=INK)
ax[1].set_ylabel("°C"); ax[1].set_title("Tank temperature: the process limit catches it"); ax[1].set_xlabel("time since START [s]")
ax[1].legend(loc="lower right")
fig.tight_layout(); fig.savefig(OUT / "plot_precool_failure.png", dpi=130); plt.close(fig)

# ---- 3. both pressure sensors frozen ----
rf, cf = load("frozen_sensors")
t = cf("t_s")
fig, ax = plt.subplots(figsize=(8, 4.2))
ax.plot(t, cf("p_true_mpa"), color=S1, label="true pressure (model)")
ax.plot(t, cf("p_meas_mpa"), color=S2, label="what both transmitters report")
ax.axvline(15, color=INK2, lw=1, ls=":")
ax.text(15.3, ax.get_ylim()[1] * 0.95, "PT1 + PT2 freeze", fontsize=9, color=INK2, va="top")
trip = first(rf, lambda r: r["state"] != "FILLING")
ax.axvline(trip, color=CRIT, lw=1.2, ls="--")
ax.text(trip + 0.3, ax.get_ylim()[1] * 0.75, "NO_RISE → STOP\n(valve open, pressure\nnot rising for 5 s)", fontsize=9, color=CRIT, va="top")
ax.set_xlabel("time since START [s]"); ax.set_ylabel("MPa"); ax.legend(loc="upper left")
ax.set_title("Two sensors that agree can both be wrong: only the process model sees it")
fig.tight_layout(); fig.savefig(OUT / "plot_frozen_sensors.png", dpi=130); plt.close(fig)

# ---- 4. NAMUR NE43 bands ----
fig, ax = plt.subplots(figsize=(8, 1.9))
bands = [(0, 3.6, "FAULT\nOPEN", CRIT), (3.6, 3.8, "", "#eda100"), (3.8, 20.5, "OK  (4 mA = 0 %, 20 mA = 100 %)", S3),
         (20.5, 21, "", "#eda100"), (21, 24, "FAULT\nSHORT", CRIT)]
for a, b, lab, col in bands:
    ax.barh(0, b - a, left=a, color=col, height=0.6, edgecolor=SURF, linewidth=2)
    if lab: ax.text((a + b) / 2, 0, lab, ha="center", va="center", color="white", fontsize=9, fontweight="bold")
ax.annotate("UNDER 3.6–3.8", (3.7, 0.3), (6, 0.75), arrowprops=dict(arrowstyle="->", color=INK2), fontsize=8, color=INK2)
ax.annotate("OVER 20.5–21", (20.75, 0.3), (15, 0.75), arrowprops=dict(arrowstyle="->", color=INK2), fontsize=8, color=INK2)
ax.set_xlim(0, 24); ax.set_ylim(-0.5, 1.1); ax.set_yticks([]); ax.grid(False)
ax.set_xticks([0, 4, 8, 12, 16, 20, 24]); ax.set_xlabel("loop current [mA]")
ax.set_title("NAMUR NE43: the current itself says whether the cable is healthy")
fig.tight_layout(); fig.savefig(OUT / "plot_ne43_bands.png", dpi=130); plt.close(fig)
print("plots written to", OUT)
