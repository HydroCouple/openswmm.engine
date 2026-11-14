#!/usr/bin/env python3
"""Render the generated review Markdown to a paginated review PDF.

Requires reportlab and Pillow; the analysis/plots are produced separately.
"""
import argparse
import html
import re
from pathlib import Path

from PIL import Image as PILImage
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.pagesizes import letter
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Image, PageBreak


def inline(text):
    text = html.escape(text)
    text = re.sub(r'`([^`]+)`',r'<font face="Courier">\1</font>',text)
    return re.sub(r'\*\*([^*]+)\*\*',r'<b>\1</b>',text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source',type=Path)
    ap.add_argument('output',type=Path)
    args = ap.parse_args()
    styles=getSampleStyleSheet()
    base=ParagraphStyle('ReviewBody',fontName='Helvetica',fontSize=9,leading=11.7,
                        textColor=colors.HexColor('#263746'),spaceAfter=6)
    small=ParagraphStyle('ReviewSmall',parent=base,fontSize=8,leading=10,spaceAfter=3)
    title=ParagraphStyle('ReviewTitle',parent=base,fontSize=21,leading=24,
                         textColor=colors.HexColor('#075768'),spaceAfter=12)
    h2=ParagraphStyle('ReviewHeading',parent=base,fontSize=12,leading=15,
                      textColor=colors.HexColor('#075768'),spaceBefore=9,spaceAfter=7,keepWithNext=True)
    cell=ParagraphStyle('ReviewCell',parent=small,fontSize=8,leading=9.8,spaceAfter=0)
    head=ParagraphStyle('ReviewTableHead',parent=cell,textColor=colors.white,fontName='Helvetica-Bold')
    numbered=ParagraphStyle('ReviewNumbered',parent=base,leftIndent=13,bulletIndent=0)
    story=[]
    lines=args.source.read_text().splitlines()
    i=0
    while i<len(lines):
        line=lines[i].strip()
        if not line:
            i+=1;continue
        if line=='<!-- pagebreak -->':
            story.append(PageBreak());i+=1;continue
        if line.startswith('# '):
            story.append(Paragraph(inline(line[2:]),title));i+=1;continue
        if line.startswith('## '):
            story.append(Paragraph(inline(line[3:]),h2));i+=1;continue
        match=re.fullmatch(r'!\[(.*?)\]\((.*?)\)',line)
        if match:
            path=args.source.parent/match[2]
            w,h=PILImage.open(path).size
            width=480 if path.name=='model_diagnostics.png' else 524
            height=width*h/w
            story.append(Image(str(path),width=width,height=height))
            story.append(Spacer(1,5));i+=1;continue
        if line.startswith('|'):
            data=[]
            while i<len(lines) and lines[i].strip().startswith('|'):
                items=[v.strip() for v in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?',v) for v in items):
                    data.append(items)
                i+=1
            widths=[524/len(data[0])]*len(data[0])
            grid=[[Paragraph(inline(v),head if row==0 else cell) for v in items] for row,items in enumerate(data)]
            tab=Table(grid,colWidths=widths,repeatRows=1,hAlign='LEFT')
            tab.setStyle(TableStyle([
                ('BACKGROUND',(0,0),(-1,0),colors.HexColor('#075768')),
                ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.HexColor('#edf5f6'),colors.white]),
                ('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),6),
                ('RIGHTPADDING',(0,0),(-1,-1),6),('TOPPADDING',(0,0),(-1,-1),5),
                ('BOTTOMPADDING',(0,0),(-1,-1),5),
                ('LINEBELOW',(0,-1),(-1,-1),.5,colors.HexColor('#b6c9ce'))]))
            story.extend([tab,Spacer(1,8)]);continue
        if line.startswith('- '):
            story.append(Paragraph(inline(line[2:]),base,bulletText='-'));i+=1;continue
        number=re.match(r'^(\d+)\. (.*)',line)
        if number:
            story.append(Paragraph(inline(number[2]),numbered,bulletText=number[1]+'.'));i+=1;continue
        paragraph=[line];i+=1
        while i<len(lines) and lines[i].strip() and not lines[i].startswith(('#','|','!','<!--','- ')) and not re.match(r'^\d+\. ',lines[i]):
            paragraph.append(lines[i].strip());i+=1
        story.append(Paragraph(inline(' '.join(paragraph)),base))

    def page(canvas,doc):
        canvas.saveState()
        canvas.setStrokeColor(colors.HexColor('#b6c9ce'))
        canvas.line(44,32,568,32)
        canvas.setFont('Helvetica',8)
        canvas.setFillColor(colors.HexColor('#597079'))
        canvas.drawString(44,20,'OpenSWMM | Rubinato 2017 comparison | Review draft, 21 October 2026')
        canvas.drawRightString(568,20,str(doc.page))
        canvas.restoreState()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    doc=SimpleDocTemplate(str(args.output),pagesize=letter,leftMargin=44,rightMargin=44,
                          topMargin=36,bottomMargin=43,title='OpenSWMM 1D/2D coupling - Rubinato 2017 review',
                          author='OpenSWMM project')
    doc.build(story,onFirstPage=page,onLaterPages=page)
    print(args.output)


if __name__=='__main__':
    main()
