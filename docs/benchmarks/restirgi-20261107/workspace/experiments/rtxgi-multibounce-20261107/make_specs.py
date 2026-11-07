"""Explicit launch recipes; GPU execution remains a separate root-owned action."""
from pathlib import Path
import argparse,json
ROOT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--version',default='v1');args=parser.parse_args()
specs=ROOT/'run-specs';specs.mkdir(exist_ok=True)
scene=str(ROOT/'shared-scene/kitchen.scene.json')
cache=ROOT.parents[1]/'cache'/ROOT.name
def emit(renderer,label,mode,frames,offset=0,warmup=0,phase='quality'):
    name=f'{renderer}-{label}-{args.version}';out=ROOT/'runs'/name
    if renderer=='rtxpt':
        capture='0,1,2,3' if label.startswith('pilot') else ','.join(map(str,[0,63]+list(range(96,128))))
        s=dict(executable=str(cache/'rtxpt/bin/Rtxpt.exe'),args=['--scene',scene,'--width','1920','--height','1080','--nonInteractive'],
            env=dict(RESTIR_REPO_BENCH_OUTPUT=str(out),RESTIR_REPO_BENCH_FRAMES=frames+warmup,RESTIR_REPO_BENCH_INTERVAL_MS=0,
                     RESTIR_GI_MODE=mode,RESTIR_GI_CAPTURE='none' if phase!='quality' else capture,RESTIR_GI_SAMPLE_OFFSET=offset))
    else:
        s=dict(executable=str(ROOT/'rtxdi-build-msvc1444/bin/FullSample.exe'),args=[
            '--scene.file',scene,'--benchmark.output',str(out/'capture'),'--benchmark.mode',phase,
            '--benchmark.giMode','both' if mode=='temporalspatial' else mode,'--benchmark.warmup',str(warmup),
            '--benchmark.measured',str(frames),'--benchmark.intervalMs','0','--benchmark.seed',str(offset)])
    s.update(output=str(out),timeout_seconds=1800,experiment=dict(renderer=renderer,label=label,mode=mode,
        frames=frames,warmup=warmup,sample_offset=offset,scene_contract=str(ROOT/'scene_contract.json')))
    p=specs/f'{name}.json'
    if not p.exists():p.write_text(json.dumps(s,indent=2),encoding='utf-8')
for renderer in ['rtxpt','rtxdi']:
    emit(renderer,'pilot-none','none',4)
    emit(renderer,'pilot-both','temporalspatial',4)
    for half,offset in [('a',100000),('b',200000)]:emit(renderer,'reference-'+half,'reference',1024,offset,phase='reference')
    for repeat,offset in enumerate([0,10000,20000],1):
        emit(renderer,f'quality-r{repeat}','temporalspatial',128,offset)
        emit(renderer,f'timing-r{repeat}','temporalspatial',240,offset,120,'timing')
print(specs)
