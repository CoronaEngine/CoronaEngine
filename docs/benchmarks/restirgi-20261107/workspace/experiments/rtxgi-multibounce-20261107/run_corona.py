"""Root-owned native Corona offline capture using the existing CEF/API harness."""
from pathlib import Path
import argparse,copy,hashlib,importlib.util,json,os,sys
from unittest.mock import patch
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parent;REPO=ROOT.parents[2]
def module(name,p):
    s=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
gi=module('corona_gi_capture',ROOT.parent/'restirgi-aligned-20261107/corona_capture.py')
pt=module('corona_pt_capture',ROOT.parent/'restirgi-pt-compare-20261107/pt_capture.py')
parser=argparse.ArgumentParser();parser.add_argument('kind',choices=['port','pt']);parser.add_argument('label')
parser.add_argument('--phase',choices=['pilot','quality','timing','reference'],default='pilot')
parser.add_argument('--offset',type=int,default=0);parser.add_argument('--frames',type=int);parser.add_argument('--config-only',action='store_true')
parser.add_argument('--diagnostic',default='')
args=parser.parse_args();runner=gi if args.kind=='port' else pt
contract=json.loads((ROOT/'scene_contract.json').read_text())
out=ROOT/'runs'/f'corona-{args.kind}-{args.label}'
frames=args.frames or (4 if args.phase=='pilot' else 128 if args.phase=='quality' else 360 if args.phase=='timing' else 1024)
checkpoints=list(range(frames)) if args.phase=='pilot' else [0,63]+list(range(96,128)) if args.phase=='quality' else [0]
c=dict(executable=str(REPO/'cmake-build-relwithdebinfo/examples/engine/corona_engine.exe'),output=str(out),scene_path=contract['corona_scene'],
       camera_id=None,port=9257,timeout_seconds=1800,checkpoints=checkpoints,reference_frames=0,reference_start_index=0,quality_mean_frames=0,
       keep_reference_raw=False,render_frames=frames,warmup_frames=120 if args.phase=='timing' else 0,primary_jitter=False,seed=0,
       capture_raw=args.phase!='timing',camera_pose=dict(contract['camera']['corona_api'],persist=False))
if args.kind=='port':
    c['settings']=dict(max_depth=16,min_depth=5,rr_threshold=1,direct=dict(open=True,M_light=1,M_bsdf=0,max_age=30,max_recursion=1,pairwise=True,diffuse_factor=.3,
        temporal=dict(open=False,history_limit=20,radius=0,theta=35,depth=.1),spatial=dict(open=False,sample_num=1,radius=30,theta=35,depth=.1)),
        indirect=dict(open=True,max_age=30,diffuse_factor=.3,temporal=dict(open=True,history_limit=5,radius=0,theta=35,depth=.3),spatial=dict(open=True,sample_num=2,radius=12,theta=35,depth=.3)))
else:c['settings']=dict(max_depth=16,min_depth=5,rr_threshold=1)
if args.phase=='reference':
    c['reference_frames']=frames;c['checkpoints']=[0,frames-1]
    if args.kind=='port':raise SystemExit('Use independent PT reference; port reference mode is not enabled')
env=dict(VISION_RESTIR_GI_MULTIBOUNCE_BENCH='1',VISION_BENCH_SAMPLE_OFFSET=str(args.offset),VISION_PT_ALIGNED_LAMBERT='1',
         VISION_RESTIR_GI_ALIGNED_LAMBERT='1',VISION_RESTIR_GI_MB_DIAGNOSTIC='1' if args.kind=='port' and args.phase=='pilot' else '0')
if args.kind=='port':env['VISION_RESTIR_GI_MODE']='full'
if args.diagnostic:env['VISION_RESTIR_GI_DIAGNOSTIC']=args.diagnostic
c['benchmark_environment']=env;c['frozen_scene_contract']=contract;c['phase']=args.phase;c['kind']=args.kind
if args.config_only:print(json.dumps(c,indent=2));raise SystemExit
original_env=runner.engine_environment
def environment(source,cache,port):
    clean={k:v for k,v in source.items() if not k.startswith(('VISION_RESTIR_','VISION_PT_','VISION_BENCH_','VISION_FALCOR_','CORONA_DI_'))}
    value=original_env(clean,cache,port);value.update(env);return value
with patch.object(runner,'engine_environment',environment):
    result=runner.run(c)
log=(out/'engine.log').read_text(encoding='utf-8',errors='replace')
expected='RestirGI multibounce benchmark:' if args.kind=='port' else 'PT multibounce benchmark active:'
route_ok=expected in log
(out/'route-validation.json').write_text(json.dumps(dict(expected_log=expected,observed=route_ok),indent=2))
if not route_ok:
    raise SystemExit(f'FAILED actual requested route absent: {expected}; run excluded')
print(json.dumps(dict(output=str(out),status=result.get('status'),error=result.get('error')),indent=2))
raise SystemExit(0 if result.get('status')=='complete' else 1)
