#!/usr/bin/env python3
#
#  plot_relay.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  Turns `build/ws_relay_scaling --csv` into docs/relay_scaling.svg.
#
#  Hand-written SVG on the Python standard library, with no matplotlib and no
#  pip install, for the same reason the library itself has no dependencies: a
#  figure CI cannot regenerate is a screenshot, and a chart that needs a
#  toolchain nobody has is a screenshot with extra steps.
#
#      ./build/ws_relay_scaling --csv > relay.csv
#      python3 scripts/plot_relay.py relay.csv docs/relay_scaling.svg
#
#  Three panels, ONE y-axis each. The temptation is to put deliveries/s and
#  MB/s on the same panel with two scales; that is the single most misleading
#  thing a chart can do, because the crossing point is an artefact of whichever
#  scales were chosen. Two measures, two panels.
#

import csv
import sys

# Validated against the six checks in both modes (worst adjacent CVD dE 24.7
# light / 26.8 dark, normal-vision 33.6 / 31.8, contrast >= 3:1 on both
# surfaces). Do not substitute by eye - re-run the validator.
LIGHT = {
    "surface": "#fcfcfb", "ink": "#1a1a19", "muted": "#63625a",
    "grid": "#e2e1db", "axis": "#c3c2b7",
    "measured": "#2a78d6", "model": "#eb6834",
}
DARK = {
    "surface": "#1a1a19", "ink": "#ffffff", "muted": "#c3c2b7",
    "grid": "#33322e", "axis": "#4a4944",
    "measured": "#3987e5", "model": "#d95926",
}

W, H = 1020, 360
PAD_L, PAD_R, PAD_T, PAD_B = 58, 30, 50, 52
PANEL_W = (W - 24) / 3.0


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def nice_ticks(lo, hi, count=4):
    if hi <= lo:
        return [lo]
    raw = (hi - lo) / count
    mag = 10 ** len(str(int(raw))) if raw >= 1 else 0.1
    for step in (mag * f for f in (0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10)):
        if step >= raw:
            break
    ticks, t = [], 0.0
    while t <= hi * 1.0001:
        if t >= lo:
            ticks.append(t)
        t += step
    ticks.append(t)              # one past the max, so the axis covers the data
    return ticks or [lo, hi]


def fmt(v):
    if v >= 1000:
        return "%.0f" % v
    if v >= 10:
        return "%.0f" % v
    return ("%.1f" % v).rstrip("0").rstrip(".")


class Panel:
    """One set of axes. x is categorical-by-position (the sweeps are not evenly
    spaced, and plotting them linearly would squash every small value into the
    left margin and hide exactly the region where the model breaks down)."""

    def __init__(self, ox, title, subtitle, xlabels, ylabel):
        self.ox, self.title, self.subtitle = ox, title, subtitle
        self.xlabels, self.ylabel = xlabels, ylabel
        self.series = []

    def add(self, name, values, role, dashed=False):
        self.series.append((name, values, role, dashed))

    def render(self, out):
        pw, ph = PANEL_W - PAD_L - PAD_R, H - PAD_T - PAD_B
        x0, y0 = self.ox + PAD_L, PAD_T
        hi = max(max(v for v in vals) for _, vals, _, _ in self.series)
        ticks = nice_ticks(0, hi)
        top = max(ticks[-1], hi)

        def px(i):
            n = len(self.xlabels)
            return x0 + (pw * (i + 0.5) / n if n > 1 else pw / 2)

        def py(v):
            return y0 + ph - (ph * v / top if top else 0)

        out.append('<text x="%.1f" y="20" class="t-title">%s</text>'
                   % (self.ox + PAD_L, esc(self.title)))
        out.append('<text x="%.1f" y="35" class="t-sub">%s</text>'
                   % (self.ox + PAD_L, esc(self.subtitle)))

        for t in ticks:                                   # recessive grid
            y = py(t)
            out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="grid"/>'
                       % (x0, y, x0 + pw, y))
            out.append('<text x="%.1f" y="%.1f" class="t-tick" text-anchor="end">%s</text>'
                       % (x0 - 8, y + 4, fmt(t)))
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="axis"/>'
                   % (x0, y0 + ph, x0 + pw, y0 + ph))
        out.append('<text x="%.1f" y="%.1f" class="t-axis" text-anchor="middle" '
                   'transform="rotate(-90 %.1f %.1f)">%s</text>'
                   % (x0 - 46, y0 + ph / 2, x0 - 46, y0 + ph / 2, esc(self.ylabel)))

        for i, lab in enumerate(self.xlabels):
            out.append('<text x="%.1f" y="%.1f" class="t-tick" text-anchor="middle">%s</text>'
                       % (px(i), y0 + ph + 18, esc(lab)))

        placed = []          # label y positions already used in this panel
        for name, vals, role, dashed in self.series:
            pts = " ".join("%.1f,%.1f" % (px(i), py(v)) for i, v in enumerate(vals))
            out.append('<polyline points="%s" class="line %s%s"/>'
                       % (pts, role, " dash" if dashed else ""))
            if not dashed:                                 # markers >= 8px, ringed
                for i, v in enumerate(vals):
                    out.append('<circle cx="%.1f" cy="%.1f" r="4.5" class="dot %s"/>'
                               % (px(i), py(v), role))
            # Direct label at the last point: identity never rests on colour
            # alone. Flip it below the point when the point is high, or it
            # collides with the panel subtitle.
            ly = py(vals[-1])
            dy = 15 if (ly - y0) < ph * 0.22 else -11
            ty = ly + dy
            # Two series ending at nearly the same value would stack their
            # labels on one another - which is what "measured" and the fitted
            # line do when the fit is good, i.e. exactly when the chart works.
            while any(abs(ty - u) < 13 for u in placed):
                ty += 14 if dy > 0 else -14
            placed.append(ty)
            out.append('<text x="%.1f" y="%.1f" class="t-series %s">%s</text>'
                       % (px(len(vals) - 1) - 6, ty, role, esc(name)))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "relay.csv"
    dst = sys.argv[2] if len(sys.argv) > 2 else "docs/relay_scaling.svg"
    rows = list(csv.DictReader(open(src)))
    if not rows:
        raise SystemExit("plot_relay: no rows in " + src)

    def series(name):
        return [r for r in rows if r["series"] == name]

    m = series("m_sweep")
    n = series("n_sweep")
    p = series("payload_sweep")
    if not (m and n and p):
        raise SystemExit("plot_relay: expected m_sweep, n_sweep and payload_sweep rows")

    # Fit a + b*M over the saturated points only - same rule as the benchmark:
    # below saturation the relay is idle between messages and the point belongs
    # to a different line.
    sat = [r for r in m if int(r["m"]) >= 50]
    xs = [float(r["m"]) for r in sat]
    ys = [float(r["per_msg_us"]) for r in sat]
    k = len(xs)
    den = k * sum(x * x for x in xs) - sum(xs) ** 2
    b = (k * sum(x * y for x, y in zip(xs, ys)) - sum(xs) * sum(ys)) / den if den else 0.0
    a = (sum(ys) - b * sum(xs)) / k if k else 0.0

    panels = []

    p1 = Panel(0, "Cost is linear in M",
               "per inbound message, N=4 · fitted a+b·M where M≥ 50",
               [r["m"] for r in m], "microseconds")
    p1.add("measured", [float(r["per_msg_us"]) for r in m], "measured")
    p1.add("a+b·M", [a + b * float(r["m"]) for r in m], "model", dashed=True)
    panels.append(p1)

    p2 = Panel(PANEL_W + 12, "Cost is flat in N",
               "per inbound message, M=50 — inbound sockets are free",
               [r["n"] for r in n], "microseconds")
    p2.add("measured", [float(r["per_msg_us"]) for r in n], "measured")
    panels.append(p2)

    p3 = Panel(2 * (PANEL_W + 12), "Throughput by payload",
               "N=4, M=50 — syscall-bound small, bandwidth-bound large",
               [("%dK" % (int(r["payload"]) // 1024)) if int(r["payload"]) >= 1024
                else r["payload"] for r in p], "MB/s out")
    p3.add("MB/s", [float(r["mb_s"]) for r in p], "measured")
    panels.append(p3)

    css = []
    for sel, c in (("", LIGHT), ("@media (prefers-color-scheme: dark)", DARK)):
        body = (
            ".bg{fill:%(surface)s}"
            ".t-title{fill:%(ink)s;font-weight:600;font-size:13px}"
            ".t-sub{fill:%(muted)s;font-size:10.5px}"
            ".t-tick{fill:%(muted)s;font-size:10px}"
            ".t-axis{fill:%(muted)s;font-size:10.5px}"
            ".grid{stroke:%(grid)s;stroke-width:1}"
            ".axis{stroke:%(axis)s;stroke-width:1}"
            ".line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}"
            ".line.measured{stroke:%(measured)s}.line.model{stroke:%(model)s}"
            ".dot.measured{fill:%(measured)s;stroke:%(surface)s;stroke-width:2}"
            ".t-series{font-size:10.5px;font-weight:600;text-anchor:end}"
            ".t-series.measured{fill:%(measured)s}.t-series.model{fill:%(model)s}"
            ".dash{stroke-dasharray:5 4}"
        ) % c
        css.append(body if not sel else "%s{%s}" % (sel, body))

    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
           'height="%d" font-family="-apple-system,BlinkMacSystemFont,Segoe UI,Helvetica,Arial,sans-serif">'
           % (W, H, W, H),
           "<style>%s</style>" % "".join(css),
           '<rect class="bg" x="0" y="0" width="%d" height="%d"/>' % (W, H)]
    for pan in panels:
        pan.render(out)
    out.append('<text x="%d" y="%d" class="t-sub">ws_broadcast_hub relay, one loop thread. '
               'Higher is worse on the left two panels, better on the right.</text>'
               % (PAD_L, H - 10))
    out.append("</svg>")

    # Overflow is the one layout fault that is invisible in a thumbnail and
    # obvious to every reader, so it is asserted rather than eyeballed.
    import re as _re
    doc = "\n".join(out)
    coords = [float(v) for v in _re.findall(r'(?:\bx|\bx1|\bx2|\bcx)="([0-9.]+)"', doc)]
    for pts in _re.findall(r'points="([^"]+)"', doc):
        coords += [float(pair.split(",")[0]) for pair in pts.split()]
    widest = max(coords) if coords else 0.0
    if widest > W:
        raise SystemExit("plot_relay: content reaches x=%.1f, past the %d viewBox - "
                         "widen W or shrink the panels" % (widest, W))

    import os
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    open(dst, "w").write(doc + "\n")
    print("plot_relay: %s -> %s  (a=%.1f us, b=%.2f us/subscriber, widest x=%.0f/%d)"
          % (src, dst, a, b, widest, W))


if __name__ == "__main__":
    main()
