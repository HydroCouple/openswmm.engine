#!/usr/bin/env python3
"""Dump a subcatchment's rainfall time series from a SWMM 5 binary .out file.

Usage: read_subcatch_rain.py <file.out> <subcatch_id>
"""
import struct
import sys

MAGIC = 516114522


def read_int(f):
    return struct.unpack("<i", f.read(4))[0]


def read_str(f):
    n = read_int(f)
    return f.read(n).decode("latin-1")


def main(path, sub_id):
    with open(path, "rb") as f:
        f.seek(-6 * 4, 2)
        id_pos, prop_pos, res_pos, n_periods, err, magic2 = struct.unpack("<6i", f.read(24))
        assert magic2 == MAGIC, "not a SWMM .out file"

        f.seek(0)
        assert read_int(f) == MAGIC
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
        f.read(4 * n_sub_props)
        f.read(4 * n_sub_props * n_subs)
        n_node_props = read_int(f)
        f.read(4 * n_node_props)
        f.read(4 * n_node_props * n_nodes)
        n_link_props = read_int(f)
        f.read(4 * n_link_props)
        f.read(4 * n_link_props * n_links)

        n_sub_vars = read_int(f)
        sub_var_codes = struct.unpack("<%di" % n_sub_vars, f.read(4 * n_sub_vars))
        n_node_vars = read_int(f)
        f.read(4 * n_node_vars)
        n_link_vars = read_int(f)
        f.read(4 * n_link_vars)
        n_sys_vars = read_int(f)
        f.read(4 * n_sys_vars)

        rain_ofs = sub_var_codes.index(0)  # SUBCATCH_RAINFALL = 0
        sub_idx = subs.index(sub_id)

        period_floats = (n_subs * n_sub_vars + n_nodes * n_node_vars
                         + n_links * n_link_vars + n_sys_vars)
        f.seek(res_pos)
        t0 = None
        for _ in range(n_periods):
            (date,) = struct.unpack("<d", f.read(8))
            vals = struct.unpack("<%df" % period_floats, f.read(4 * period_floats))
            if t0 is None:
                t0 = date
            rain = vals[sub_idx * n_sub_vars + rain_ofs]
            print("%10.4f  %.6f  %.6f" % ((date - t0) * 24.0, date, rain))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
