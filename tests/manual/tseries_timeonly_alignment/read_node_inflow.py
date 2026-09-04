#!/usr/bin/env python3
"""Dump a node's total-inflow time series from a SWMM 5 binary .out file.

Usage: read_node_inflow.py <file.out> <node_id>

Minimal reader for the legacy .out layout (shared by openswmm's legacy-
compatible writer). Prints "elapsed_hours  datetime_serial  inflow" rows.
"""
import struct
import sys

MAGIC = 516114522


def read_int(f):
    return struct.unpack("<i", f.read(4))[0]


def read_str(f):
    n = read_int(f)
    return f.read(n).decode("latin-1")


def main(path, node_id):
    with open(path, "rb") as f:
        f.seek(-6 * 4, 2)
        id_pos, prop_pos, res_pos, n_periods, err, magic2 = struct.unpack("<6i", f.read(24))
        assert magic2 == MAGIC, "not a SWMM .out file"

        f.seek(0)
        magic1 = read_int(f)
        assert magic1 == MAGIC
        _version = read_int(f)
        _flow_units = read_int(f)
        n_subs = read_int(f)
        n_nodes = read_int(f)
        n_links = read_int(f)
        n_polls = read_int(f)

        f.seek(id_pos)
        subs = [read_str(f) for _ in range(n_subs)]
        nodes = [read_str(f) for _ in range(n_nodes)]
        links = [read_str(f) for _ in range(n_links)]
        _polls = [read_str(f) for _ in range(n_polls)]
        _poll_units = [read_int(f) for _ in range(n_polls)]

        f.seek(prop_pos)
        n_sub_props = read_int(f)
        f.read(4 * n_sub_props)  # prop codes
        f.read(4 * n_sub_props * n_subs)
        n_node_props = read_int(f)
        f.read(4 * n_node_props)
        f.read(4 * n_node_props * n_nodes)
        n_link_props = read_int(f)
        f.read(4 * n_link_props)
        f.read(4 * n_link_props * n_links)

        n_sub_vars = read_int(f)
        f.read(4 * n_sub_vars)
        n_node_vars = read_int(f)
        node_var_codes = struct.unpack("<%di" % n_node_vars, f.read(4 * n_node_vars))
        n_link_vars = read_int(f)
        f.read(4 * n_link_vars)
        n_sys_vars = read_int(f)
        f.read(4 * n_sys_vars)

        # NODE_INFLOW (total inflow) is var code 4 in legacy enums
        inflow_ofs = node_var_codes.index(4)
        node_idx = nodes.index(node_id)

        period_floats = (n_subs * n_sub_vars + n_nodes * n_node_vars
                         + n_links * n_link_vars + n_sys_vars)
        f.seek(res_pos)
        t0 = None
        for _ in range(n_periods):
            (date,) = struct.unpack("<d", f.read(8))
            vals = struct.unpack("<%df" % period_floats, f.read(4 * period_floats))
            if t0 is None:
                t0 = date
            inflow = vals[n_subs * n_sub_vars + node_idx * n_node_vars + inflow_ofs]
            print("%10.4f  %.6f  %.6f" % ((date - t0) * 24.0, date, inflow))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
