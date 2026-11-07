"""Single sequential uncapped GPU queue for the fifth implementation only."""
from pathlib import Path
import datetime,json,subprocess,sys
R=Path(__file__).resolve().parent
records=[]
for phase in ['quality','timing']:
    for repeat,offset in enumerate([0,10000,20000],1):
        label=f'{phase}-r{repeat}-v1'
        command=[sys.executable,str(R/'run_native.py'),label,'--phase',phase,'--offset',str(offset)]
        print('START corona-native-'+label,flush=True)
        start=datetime.datetime.now(datetime.timezone.utc).isoformat()
        code=subprocess.run(command,cwd=R.parents[2]).returncode
        records.append(dict(command=command,exit_code=code,start_utc=start,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()))
        (R/'suite-v1.json').write_text(json.dumps(records,indent=2))
        if code:raise SystemExit(code)
        print('DONE corona-native-'+label,flush=True)
