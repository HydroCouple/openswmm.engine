#!/usr/bin/env python3
"""Publish curated momentum-study data/figures; never include native run outputs."""
import argparse
from pathlib import Path
import shutil
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

ROOT=Path(__file__).resolve().parents[3]
NAMES=['hydraulics_ch9_rubinato_momentum_parity',
       'hydraulics_ch9_rubinato_momentum_convergence']


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--gui-root', type=Path)
    args=ap.parse_args()
    report=ROOT/'docs/validation/rubinato2017'
    downloads=ROOT/'docs/manuals/reference/hydraulics/downloads'
    payload={}
    for name in ['momentum_report.md','momentum_comparison.csv','momentum_data.json']:
        p=report/name;payload[str(p.relative_to(ROOT))]=p.read_bytes()
    for name in NAMES:
        for ext, folder in [('png','png'), ('svg','svg')]:
            p=ROOT/'docs/figures'/folder/(name+'.'+ext)
            payload[str(p.relative_to(ROOT))]=p.read_bytes()
    payload['README.txt']=(
        'Rubinato momentum smoothing follow-up, 22 October 2026.\n'
        'Open docs/validation/rubinato2017/momentum_report.md; PNG/SVG figures and\n'
        'all plotted data are included. No raw simulation outputs or logs.\n'
        'Experimental agreement remains incomplete; convergence is assessed separately.\n'
        'Reference: Rubinato et al. (2017), Journal of Hydrology 552, 421-432.\n'
        'doi:10.1016/j.jhydrol.2017.06.024. Reference observations: CC-BY-4.0.\n'
    ).encode()
    archive=downloads/'rubinato2017_momentum.zip'
    with ZipFile(archive,'w',compression=ZIP_DEFLATED,compresslevel=9) as z:
        for name, content in sorted(payload.items()):
            info=ZipInfo(name,date_time=(1980,1,1,0,0,0));info.compress_type=ZIP_DEFLATED
            info.external_attr=0o100644 << 16;z.writestr(info,content)
    for source, target in [('momentum_comparison.csv','rubinato2017_momentum_comparison.csv'),
                            ('momentum_data.json','rubinato2017_momentum_data.json')]:
        shutil.copy2(report/source,downloads/target)
    if args.gui_root:
        gui=args.gui_root.resolve();assets=gui/'docs/manual/tutorials/models'
        for name in [archive.name,'rubinato2017_momentum_comparison.csv','rubinato2017_momentum_data.json']:
            shutil.copy2(downloads/name,assets/name)
        for name in NAMES:
            shutil.copy2(ROOT/'docs/figures/png'/(name+'.png'),
                         gui/'docs/manual/images'/(name.replace('hydraulics_ch9_', 't11_')+'.png'))
    print('Published report, two figures and plotted data; no simulation outputs.')


if __name__ == '__main__':
    main()
