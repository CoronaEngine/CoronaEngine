"""Add original-app launch specs; preserve already-executed pilot descriptions."""
from pathlib import Path
import json
R=Path(__file__).resolve().parent;cache=R.parents[1]/'cache'/R.name
base=json.loads((R/'run-specs/reference-pilot-v3.json').read_text(encoding='utf-8-sig'))
def make(label,frames,warmup,offset,raw,reference=False):
    out=R/'runs'/('reference-'+label)
    a=base['args'][:]
    for key,val in [('--output',str(out)),('--frames',str(frames)),('--warmup',str(warmup)),('--sample-offset',str(offset))]:a[a.index(key)+1]=val
    i=a.index('--raw-frames');del a[i:i+2]
    if not raw:a.remove('--export-raw')
    else:a+=['--raw-frames',str(frames-1) if reference else ','.join(map(str,[0,63]+list(range(96,128))))]
    if reference:a+=['--reference']
    a+=['--min-depth','5','--max-depth','16']
    s=dict(executable=str(cache/'reference/bin-release/RestirGI.exe'),args=a,output=str(out),timeout_seconds=1800,
           experiment=dict(renderer='originalRestirGI',label=label,scene_contract=str(R/'scene_contract.json'),frames=frames,warmup=warmup,sample_offset=offset))
    p=R/'run-specs'/('reference-'+label+'.json')
    if not p.exists():p.write_text(json.dumps(s,indent=2))
for half,offset in [('a',100000),('b',200000)]:make('reference-'+half+'-v1',1024,0,offset,True,True)
for i,offset in enumerate([0,10000,20000],1):
    make(f'quality-r{i}-v1',128,0,offset,True)
    make(f'timing-r{i}-v1',360,120,offset,False)
print('Original formal run specs ready')
