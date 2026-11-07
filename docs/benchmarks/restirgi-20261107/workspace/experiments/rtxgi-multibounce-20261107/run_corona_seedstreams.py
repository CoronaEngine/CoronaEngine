"""Recollect Corona with all sampling streams shifted together and logical phases unchanged."""
from pathlib import Path
import datetime,json,subprocess,sys
R=Path(__file__).resolve().parent
records=[]
for phase in ['quality','timing']:
    for repeat,offset in enumerate([0,10000,20000],1):
        label=f'{phase}-r{repeat}-seedstreams-v1'
        output=R/'runs'/f'corona-port-{label}'
        if output.exists():raise SystemExit(f'Output already exists: {output}')
        command=[sys.executable,str(R/'run_corona.py'),'port',label,'--phase',phase,'--offset',str(offset)]
        print('START '+output.name,flush=True)
        started=datetime.datetime.now(datetime.timezone.utc).isoformat()
        code=subprocess.run(command,cwd=R.parents[2]).returncode
        records.append(dict(command=command,output=str(output),exit_code=code,start_utc=started,
            end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))
        (R/'suite-corona-seedstreams-v1.json').write_text(json.dumps(records,indent=2))
        if code:raise SystemExit(code)
        print('DONE '+output.name,flush=True)
