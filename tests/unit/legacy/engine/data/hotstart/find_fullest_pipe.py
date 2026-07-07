"""Find the link with the highest depth-to-full-depth ratio (capacity).

Reads variable 4 (capacity = filling ratio for conduits) from the SWMM .out
binary using swmmtoolbox and prints links ranked by max capacity.
"""
from __future__ import annotations

from pathlib import Path

from swmmtoolbox import swmmtoolbox

OUT = Path(__file__).with_name("site_drainage_model.out")

# Pull max capacity (variable 4 = "Capacity" per SWMM .out spec) for each link.
df = swmmtoolbox.extract(str(OUT), "link,,Capacity")

# Columns are named "link_<id>_Capacity"; max along time axis gives the peak ratio.
peaks = {
    col.split("_")[1]: float(df[col].max())
    for col in df.columns
}

# Also pull peak flow and peak depth for context.
flow_df = swmmtoolbox.extract(str(OUT), "link,,Flow_rate")
depth_df = swmmtoolbox.extract(str(OUT), "link,,Flow_depth")

ranked = sorted(peaks.items(), key=lambda kv: kv[1], reverse=True)

print(f"{'Link':<6}{'Max Capacity':>14}{'Peak Flow (cfs)':>20}{'Peak Depth (ft)':>20}")
print("-" * 60)
for lid, cap in ranked:
    flow = float(flow_df[f"link_{lid}_Flow_rate"].max())
    depth = float(depth_df[f"link_{lid}_Flow_depth"].max())
    print(f"{lid:<6}{cap:>14.4f}{flow:>20.3f}{depth:>20.3f}")
