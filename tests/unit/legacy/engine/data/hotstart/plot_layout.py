"""Build a Plotly figure of the site_drainage_model network layout.

Parses [COORDINATES], [VERTICES], [Polygons], [SYMBOLS], [SUBCATCHMENTS],
[JUNCTIONS], [OUTFALLS] and [CONDUITS] from the SWMM .inp file and writes
a self-contained interactive HTML figure next to the input file.
"""
from __future__ import annotations

import re
from collections import defaultdict
from pathlib import Path

import plotly.graph_objects as go

INP = Path(__file__).with_name("site_drainage_model.inp")
HTML = Path(__file__).with_name("site_drainage_model_layout.html")


def parse_sections(text: str) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        m = re.match(r"\s*\[([^\]]+)\]\s*$", line)
        if m:
            current = m.group(1).upper()
            sections[current] = []
            continue
        if current is None:
            continue
        s = line.strip()
        if not s or s.startswith(";"):
            continue
        sections[current].append(line)
    return sections


def main() -> None:
    sections = parse_sections(INP.read_text())

    # Nodes (junctions + outfalls share [COORDINATES]).
    coords: dict[str, tuple[float, float]] = {}
    for line in sections.get("COORDINATES", []):
        parts = line.split()
        coords[parts[0]] = (float(parts[1]), float(parts[2]))

    junctions = {ln.split()[0] for ln in sections.get("JUNCTIONS", [])}
    outfalls = {ln.split()[0] for ln in sections.get("OUTFALLS", [])}

    # Conduits: from -> to.
    conduits: dict[str, tuple[str, str]] = {}
    for line in sections.get("CONDUITS", []):
        parts = line.split()
        conduits[parts[0]] = (parts[1], parts[2])

    # Vertices for links (in order of appearance).
    vertices: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for line in sections.get("VERTICES", []):
        parts = line.split()
        vertices[parts[0]].append((float(parts[1]), float(parts[2])))

    # Subcatchment polygons.
    polygons: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for line in sections.get("POLYGONS", []):
        parts = line.split()
        polygons[parts[0]].append((float(parts[1]), float(parts[2])))

    # Subcatchment outlets (for connector lines).
    sub_outlet: dict[str, str] = {}
    for line in sections.get("SUBCATCHMENTS", []):
        parts = line.split()
        sub_outlet[parts[0]] = parts[2]

    # Rain gage symbol.
    gages: dict[str, tuple[float, float]] = {}
    for line in sections.get("SYMBOLS", []):
        parts = line.split()
        gages[parts[0]] = (float(parts[1]), float(parts[2]))

    fig = go.Figure()

    # --- Subcatchment polygons (filled) ---
    sub_color = "rgba(120, 180, 110, 0.25)"
    sub_line = "rgba(70, 130, 60, 0.9)"
    for sid, poly in polygons.items():
        xs = [p[0] for p in poly] + [poly[0][0]]
        ys = [p[1] for p in poly] + [poly[0][1]]
        fig.add_trace(
            go.Scatter(
                x=xs,
                y=ys,
                mode="lines",
                fill="toself",
                fillcolor=sub_color,
                line=dict(color=sub_line, width=1.5),
                name="Subcatchments",
                legendgroup="subcatchments",
                showlegend=(sid == next(iter(polygons))),
                hoverinfo="text",
                hovertext=f"Subcatchment {sid}",
            )
        )

    # Subcatchment centroid + label.
    sub_cx, sub_cy, sub_text = [], [], []
    for sid, poly in polygons.items():
        cx = sum(p[0] for p in poly) / len(poly)
        cy = sum(p[1] for p in poly) / len(poly)
        sub_cx.append(cx)
        sub_cy.append(cy)
        sub_text.append(sid)
    fig.add_trace(
        go.Scatter(
            x=sub_cx,
            y=sub_cy,
            mode="text",
            text=sub_text,
            textfont=dict(size=14, color="#2e6b1f", family="Arial Black"),
            name="Subcatchment labels",
            legendgroup="subcatchments",
            showlegend=False,
            hoverinfo="skip",
        )
    )

    # --- Subcatchment -> outlet dashed connectors ---
    conn_x: list[float | None] = []
    conn_y: list[float | None] = []
    for sid, outlet in sub_outlet.items():
        if sid not in polygons or outlet not in coords:
            continue
        cx = sum(p[0] for p in polygons[sid]) / len(polygons[sid])
        cy = sum(p[1] for p in polygons[sid]) / len(polygons[sid])
        ox, oy = coords[outlet]
        conn_x.extend([cx, ox, None])
        conn_y.extend([cy, oy, None])
    fig.add_trace(
        go.Scatter(
            x=conn_x,
            y=conn_y,
            mode="lines",
            line=dict(color="rgba(120, 120, 120, 0.6)", width=1, dash="dot"),
            name="Subcatchment outlets",
            hoverinfo="skip",
        )
    )

    # --- Conduits ---
    conduit_color = "#1f77b4"
    for lid, (u, v) in conduits.items():
        if u not in coords or v not in coords:
            continue
        path = [coords[u], *vertices.get(lid, []), coords[v]]
        xs = [p[0] for p in path]
        ys = [p[1] for p in path]
        fig.add_trace(
            go.Scatter(
                x=xs,
                y=ys,
                mode="lines",
                line=dict(color=conduit_color, width=2.5),
                name="Conduits",
                legendgroup="conduits",
                showlegend=(lid == next(iter(conduits))),
                hoverinfo="text",
                hovertext=f"Conduit {lid}: {u} → {v}",
            )
        )
        # Midpoint label.
        mid_idx = len(path) // 2
        if len(path) % 2 == 0:
            mx = (path[mid_idx - 1][0] + path[mid_idx][0]) / 2
            my = (path[mid_idx - 1][1] + path[mid_idx][1]) / 2
        else:
            mx, my = path[mid_idx]
        fig.add_trace(
            go.Scatter(
                x=[mx],
                y=[my],
                mode="text",
                text=[lid],
                textfont=dict(size=10, color=conduit_color, family="Arial"),
                textposition="top center",
                legendgroup="conduits",
                showlegend=False,
                hoverinfo="skip",
            )
        )

    # --- Junctions ---
    j_x = [coords[n][0] for n in junctions if n in coords]
    j_y = [coords[n][1] for n in junctions if n in coords]
    j_t = [n for n in junctions if n in coords]
    fig.add_trace(
        go.Scatter(
            x=j_x,
            y=j_y,
            mode="markers+text",
            marker=dict(
                size=12,
                color="#ff7f0e",
                line=dict(color="black", width=1),
                symbol="circle",
            ),
            text=j_t,
            textposition="top right",
            textfont=dict(size=11, color="black"),
            name="Junctions",
            hoverinfo="text",
            hovertext=[f"Junction {n}" for n in j_t],
        )
    )

    # --- Outfalls ---
    o_x = [coords[n][0] for n in outfalls if n in coords]
    o_y = [coords[n][1] for n in outfalls if n in coords]
    o_t = [n for n in outfalls if n in coords]
    fig.add_trace(
        go.Scatter(
            x=o_x,
            y=o_y,
            mode="markers+text",
            marker=dict(
                size=16,
                color="#d62728",
                line=dict(color="black", width=1.2),
                symbol="square",
            ),
            text=o_t,
            textposition="top right",
            textfont=dict(size=12, color="black", family="Arial Black"),
            name="Outfalls",
            hoverinfo="text",
            hovertext=[f"Outfall {n}" for n in o_t],
        )
    )

    # --- Rain gage ---
    if gages:
        g_x = [v[0] for v in gages.values()]
        g_y = [v[1] for v in gages.values()]
        g_t = list(gages.keys())
        fig.add_trace(
            go.Scatter(
                x=g_x,
                y=g_y,
                mode="markers+text",
                marker=dict(
                    size=18,
                    color="#9467bd",
                    line=dict(color="black", width=1),
                    symbol="diamond",
                ),
                text=g_t,
                textposition="bottom center",
                textfont=dict(size=11, color="#4b2a78"),
                name="Rain gage",
                hoverinfo="text",
                hovertext=[f"Rain gage {n}" for n in g_t],
            )
        )

    fig.update_layout(
        title=dict(
            text="Site Drainage Model — Network Layout",
            x=0.5,
            xanchor="center",
            font=dict(size=18),
        ),
        xaxis=dict(title="X (ft)", scaleanchor="y", scaleratio=1, zeroline=False),
        yaxis=dict(title="Y (ft)", zeroline=False),
        plot_bgcolor="white",
        width=1100,
        height=900,
        legend=dict(
            yanchor="top",
            y=0.99,
            xanchor="left",
            x=0.01,
            bgcolor="rgba(255,255,255,0.85)",
            bordercolor="black",
            borderwidth=1,
        ),
        hovermode="closest",
    )
    fig.update_xaxes(showgrid=True, gridcolor="rgba(200,200,200,0.4)")
    fig.update_yaxes(showgrid=True, gridcolor="rgba(200,200,200,0.4)")

    fig.write_html(str(HTML), include_plotlyjs="cdn", full_html=True)
    print(f"Wrote {HTML}")


if __name__ == "__main__":
    main()
