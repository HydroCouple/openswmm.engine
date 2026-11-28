"""Drive both 2D domains at exact exchange times through their runtime API.

The input model must have an active aquifer and the selected transported
species. Example: python runtime_2d_coupling.py model.inp --until 60
Add --temperature 12 --heat-w 100 when temperature transport is enabled.
"""
from argparse import ArgumentParser
from dataclasses import asdict
import json
from pathlib import Path
from openswmm.engine import Solver, RuntimeSource


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('model', type=Path)
    parser.add_argument('--until', type=float, default=60)
    parser.add_argument('--interval', type=float, default=10)
    parser.add_argument('--surface-cell', type=int, default=0)
    parser.add_argument('--groundwater-cell', type=int, default=0)
    parser.add_argument('--flow-m3-s', type=float, default=.001)
    parser.add_argument('--species', default='TSS')
    parser.add_argument('--concentration', type=float, default=5)
    parser.add_argument('--temperature', type=float)
    parser.add_argument('--heat-w', type=float, default=0)
    args = parser.parse_args()
    if args.interval <= 0 or args.until <= 0:
        parser.error('interval and until must be positive')
    stem = args.model.with_suffix('')
    with Solver(args.model, f'{stem}_runtime.rpt', f'{stem}_runtime.out') as solver:
        flags = solver.coupling.capabilities
        if not flags['surface_sources'] or not flags['groundwater_sources']:
            raise RuntimeError('This example requires the CPU surface solver and an active aquifer')
        concentrations = {args.species: args.concentration}
        if args.temperature is not None:
            concentrations['__TEMPERATURE__'] = args.temperature
        elapsed = 0.0
        previous = {'surface': 0.0, 'groundwater': 0.0}
        while elapsed < args.until:
            target = min(elapsed + args.interval, args.until)
            # Each record is a total rate for its selected cell. All inputs
            # take effect atomically from this exchange endpoint.
            solver.coupling.set_sources([
                RuntimeSource('provider', 'surface', args.surface_cell,
                              args.flow_m3_s, concentrations, heat_w=args.heat_w,
                              until_seconds=target),
                RuntimeSource('provider', 'groundwater', args.groundwater_cell,
                              args.flow_m3_s, concentrations, heat_w=args.heat_w,
                              until_seconds=target),
            ])
            solver.advance_to(target)
            receipts = {
                'surface': solver.surface2d.source_receipt('provider', args.surface_cell),
                'groundwater': solver.groundwater2d.source_receipt('provider', args.groundwater_cell),
            }
            print(json.dumps({'elapsed_seconds': target, 'exchange_m3': {
                domain: receipt.applied_m3 - previous[domain]
                for domain, receipt in receipts.items()
            }, 'receipts': {domain: asdict(receipt) for domain, receipt in receipts.items()}}))
            previous = {domain: receipt.applied_m3 for domain, receipt in receipts.items()}
            elapsed = target


if __name__ == '__main__':
    main()
