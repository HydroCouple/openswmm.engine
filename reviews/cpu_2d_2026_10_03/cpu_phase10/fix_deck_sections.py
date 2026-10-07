from pathlib import Path
p=Path(__file__).resolve().parent;f=p/'prepare_models.py';s=f.read_text().replace("a=s.index('[2D_OPTIONS]')", "a=re.search(r'^\\[2D_OPTIONS\\][ \\t]*$',s,re.M).start()")
f.write_text(s)
