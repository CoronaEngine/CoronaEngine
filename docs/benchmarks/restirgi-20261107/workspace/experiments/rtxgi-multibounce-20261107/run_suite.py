"""Single root-owned GPU queue; no renderer runs concurrently and no frame pacing."""
from pathlib import Path
import argparse,datetime,hashlib,json,subprocess,sys
R=Path(__file__).resolve().parent
p=argparse.ArgumentParser();p.add_argument('group',choices=['references','corona-reference','quality','timing']);a=p.parse_args()
tasks=[]
if a.group=='references':
    for half in ['a','b']:
        tasks += [('external',f'{v}-reference-{half}-v1',[]) for v in ['rtxpt','rtxdi','reference']]
elif a.group=='corona-reference':
    for half,offset in [('a',100000),('b',200000)]:tasks.append(('corona-pt',f'reference-{half}-v1',['--phase','reference','--offset',str(offset)]))
else:
    orders=[['rtxpt','rtxdi','reference','corona-port'],['corona-port','reference','rtxdi','rtxpt'],['reference','rtxpt','corona-port','rtxdi']]
    for repeat,(order,offset) in enumerate(zip(orders,[0,10000,20000]),1):
        for v in order:
            label=f'{a.group}-r{repeat}-v1'
            tasks.append((v if v.startswith('corona-') else 'external',label if v.startswith('corona-') else f'{v}-{label}',
                          ['--phase',a.group,'--offset',str(offset)] if v.startswith('corona-') else []))
records=[]
for kind,label,extra in tasks:
    if kind=='external':
        cmd=[sys.executable,str(R/'run_process.py'),str(R/'run-specs'/f'{label}.json')]
        out=R/'runs'/label
    else:
        role=kind.split('-',1)[1]
        cmd=[sys.executable,str(R/'run_corona.py'),role,label,*extra]
        out=R/'runs'/f'{kind}-{label}'
    if out.exists():raise SystemExit(f'Existing output must be reviewed before resuming: {out}')
    print('START '+out.name,flush=True)
    start=datetime.datetime.now(datetime.timezone.utc).isoformat()
    run=subprocess.run(cmd,cwd=R.parents[2])
    records.append(dict(output=str(out),command=cmd,exit_code=run.returncode,start_utc=start,
        end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))
    proof=dict(group=a.group,script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),runs=records)
    (R/f'suite-{a.group}.json').write_text(json.dumps(proof,indent=2),encoding='utf-8')
    if run.returncode:raise SystemExit(run.returncode)
    print('DONE '+out.name,flush=True)
