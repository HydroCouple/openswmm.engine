#!/usr/bin/env python3
"""Package the archived result figures for review and both Doxygen manuals.

Run the figure generator first. Requires reportlab and Pillow for the printable
supplement. The offline HTML embeds the same committed plotting snapshot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ENGINE = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SLUGS = ['parity', 'residuals', 'routing_steps', 'transient']
CAPTIONS = [
    'All 36 Table 2 measurements. Horizontal bars show the published xi_f variation. '
    'Axes use different scales between regimes; each panel has equal x and y scales. '
    'Paper equations use measured heads; routed models evolve their heads. '
    'The physical-shaft results use a 0.5 s routing step and are not converged.',
    'Residuals use discharge magnitudes. Green shading is the illustrative band '
    '+/-(xi_f + 2% of measurement), not a statistical confidence interval. '
    'Only 8/36 physical-shaft rates meet it. Row numbers restart within each regime.',
    'One case per regime, selected by largest coarse-step window drift. Green lines '
    'and bands show the signed measured discharge and xi_f. Blue points are the '
    'late-window means. Positive is sewer to surface. Two steps do not establish convergence.',
    'Synthetic T9 only; no measured transient curve. Surface maps use actual saved '
    'cell depths with a fixed 0-25 mm scale. The orange ring is the manhole. '
    'Frames are spaced 10 s apart and omit brief between-frame peaks. '
    'Surface inflow continues throughout the run; the map is not surcharge water alone.'
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--gui-root', type=Path, required=True)
    args = ap.parse_args()
    review = ENGINE/'docs/validation/rubinato2017'
    out = review/'visuals'
    out.mkdir(exist_ok=True)
    source = ENGINE/'docs/figures/data/rubinato2017.json'
    template = (HERE/'results_viewer.html').read_text()
    assert template.count('__RESULTS_JSON__') == 1
    viewer = out/'rubinato2017_results.html'
    viewer.write_text(template.replace('__RESULTS_JSON__', source.read_text().strip()))
    gui_images = args.gui_root/'docs/manual/images'
    for slug in SLUGS:
        for ext in ('png', 'svg'):
            name = 'hydraulics_ch9_rubinato_'+slug+'.'+ext
            shutil.copyfile(ENGINE/'docs/figures'/ext/name, out/name)
        shutil.copyfile(out/f'hydraulics_ch9_rubinato_{slug}.png', gui_images/f't11_rubinato_{slug}.png')
    # One plot per page, with a readable interpretation and a stable evidence date.
    from reportlab.pdfgen import canvas
    from reportlab.lib.utils import ImageReader
    from reportlab.lib import colors
    from reportlab.lib.styles import ParagraphStyle
    from reportlab.platypus import Paragraph
    from PIL import Image
    pdf = out/'rubinato2017_visual_supplement.pdf'
    c = canvas.Canvas(str(pdf), pagesize=(612, 792), invariant=1)
    c.setTitle('Rubinato 2017 - Experimental versus simulated results')
    c.setAuthor('OpenSWMM')
    style = ParagraphStyle('Caption', fontName='Helvetica', fontSize=10, leading=14,
                           textColor=colors.HexColor('#193447'))
    for i, (slug, caption) in enumerate(zip(SLUGS, CAPTIONS), 1):
        c.setFillColor(colors.HexColor('#193447')); c.setFont('Helvetica-Bold', 11)
        c.drawString(44, 753, 'OpenSWMM | Rubinato 2017 | Visual comparison supplement')
        c.setFont('Helvetica', 9)
        c.drawString(44, 735, '21 October 2026 - Review material; experimental qualification incomplete')
        img = out/f'hydraulics_ch9_rubinato_{slug}.png'
        w, h = Image.open(img).size
        width, height = 524, 524*h/w
        c.drawImage(ImageReader(str(img)), 44, 710-height, width=width, height=height)
        p = Paragraph(caption, style); _, ph = p.wrap(524, 160)
        p.drawOn(c, 44, 690-height-ph)
        c.setFont('Helvetica', 8)
        c.drawString(44, 52, 'Measured source: Rubinato et al. (2017), Journal of Hydrology 552, 421-432, Table 2.')
        c.drawString(44, 40, 'DOI: 10.1016/j.jhydrol.2017.06.024 | CC BY 4.0. Native model sources recorded in the plotted-data JSON.')
        c.drawRightString(568, 25, str(i)); c.showPage()
    c.save()
    (out/'README.txt').write_text(
        'Rubinato 2017 comparison - visual supplement, 21 October 2026\n\n'
        'Open rubinato2017_results.html in a browser for steady comparisons and T9 playback.\n'
        'The HTML is self-contained and works offline. Its Download button exports plotted data.\n'
        'PNG and SVG figures are identical to the manual figures; the PDF is printable review material.\n'
        'Experimental qualification is incomplete. T9 forcing is synthetic, not measured transient data.\n'
        'Source measurements: Rubinato et al. (2017), Table 2, DOI 10.1016/j.jhydrol.2017.06.024, CC BY 4.0.\n'
        'See plotted_data.json for source hashes, numerical assumptions and binary identifiers.\n')
    shutil.copyfile(source, out/'plotted_data.json')
    shutil.copyfile(review/'steady_comparison.csv', out/'steady_comparison.csv')
    files = sorted(p for p in out.iterdir() if p.is_file() and p.suffix != '.zip')
    archive = out/'rubinato2017_visuals.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in files:
            info = zipfile.ZipInfo('rubinato2017_visuals/'+p.name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            z.writestr(info, p.read_bytes())
    for dest in [ENGINE/'docs/manuals/reference/hydraulics/downloads',
                 args.gui_root/'docs/manual/tutorials/models']:
        for p in [viewer, pdf, archive]:
            shutil.copyfile(p, dest/p.name)
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files+[archive]}
    print(json.dumps(hashes, indent=2))


if __name__ == '__main__':
    main()
