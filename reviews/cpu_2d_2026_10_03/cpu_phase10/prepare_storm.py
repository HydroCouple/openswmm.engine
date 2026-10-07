from pathlib import Path
import re,json,hashlib
p=Path(__file__).resolve().parent;original=p/'engine_decks/bellinge_10min_swe2/model.inp';s=original.read_text();changes={'START_TIME':'06:13:00','REPORT_START_TIME':'06:13:00','END_TIME':'06:23:00'}
for k,v in changes.items():s=re.sub('^'+k+r'\s+.*$',k+' '+v,s,flags=re.M)
d=p/'engine_decks/bellinge_storm_swe2';d.mkdir(exist_ok=True);(d/'model.inp').write_text(s)
(p/'bellinge_storm_provenance.json').write_text(json.dumps(dict(parent='bellinge_provenance.json',changes=changes,sha256=hashlib.sha256(s.encode()).hexdigest(),selection='Largest rolling ten-minute rainfall total in rg5425 on 2012-06-29: 9 mm. Cold-start hydraulic state at the interval start.'),indent=2))
f=p/'check_models.py';s=f.read_text().replace("'bellinge_10min_swe2' in sys.argv","any('bellinge' in x for x in sys.argv[1:])");f.write_text(s)
f=p/'verify_apply.py';s=f.read_text().replace('assert len(bellinge)==2','assert len(bellinge)==4');f.write_text(s)
f=p/'make_report.py';s=f.read_text().replace('22 complete-model executions (11 paired comparisons)','24 complete-model executions (12 paired comparisons)')
s=s.replace('Its end-to-end timing includes substantial work outside the optimized face kernels.', 'That first interval is dry (zero surface water ledger), so its timing is a dry-control result. An additional cold-start storm interval, 06:13–06:23, covers the largest rolling ten-minute gauge-5425 rainfall total that day (9 mm); see `bellinge_storm_provenance.json`. Both intervals are qualified for numerical equivalence. The storm pair is not used to claim a repeated performance gain. End-to-end timing includes substantial work outside the optimized face kernels.')
s=s.replace('and Bellinge order 2.', 'and both dry and storm Bellinge order-2 intervals.')
f.write_text(s)
