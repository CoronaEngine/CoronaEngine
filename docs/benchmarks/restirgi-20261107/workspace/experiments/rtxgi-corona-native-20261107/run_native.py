"""Root-only GPU launcher: original Corona GI, matched physical contract, no port route."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,sys
from unittest.mock import patch
sys.dont_write_bytecode=True
R=Path(__file__).resolve().parent;REPO=R.parents[2]
p=argparse.ArgumentParser();p.add_argument('label');p.add_argument('--phase',choices=['pilot','quality','timing'],default='pilot');p.add_argument('--offset',type=int,default=0);p.add_argument('--frames',type=int);p.add_argument('--fresh',action='store_true');a=p.parse_args()
spec=importlib.util.spec_from_file_location('native_capture',R.parent/'restirgi-aligned-20261107/corona_capture.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
contract=json.loads((R/'scene_contract.json').read_text(encoding='utf-8'))
frames=a.frames or (4 if a.phase=='pilot' else 128 if a.phase=='quality' else 360)
out=R/'runs'/('corona-native-'+a.label)
if out.exists():raise SystemExit('Preserve existing output: '+str(out))
c=dict(executable=str(REPO/'cmake-build-relwithdebinfo/examples/engine/corona_engine.exe'),output=str(out),scene_path=contract['corona_scene'],camera_id=None,port=9257,timeout_seconds=1800,
    checkpoints=list(range(frames)) if a.phase=='pilot' else [0,63]+list(range(96,128)) if a.phase=='quality' else [0],reference_frames=0,reference_start_index=0,quality_mean_frames=0,keep_reference_raw=False,render_frames=frames,
    warmup_frames=120 if a.phase=='timing' else 0,primary_jitter=False,seed=0,capture_raw=a.phase!='timing',camera_pose=dict(contract['camera']['corona_api'],persist=False),phase=a.phase,kind='native',frozen_scene_contract=contract)
c['settings']=dict(max_depth=16,min_depth=5,rr_threshold=1,
    direct=dict(open=True,M_light=1,M_bsdf=1,max_age=30,max_recursion=1,pairwise=True,diffuse_factor=.3,
        temporal=dict(open=False,history_limit=20,radius=0,theta=35,depth=.1),spatial=dict(open=False,sample_num=1,radius=30,theta=35,depth=.1)),
    indirect=dict(open=True,max_age=30,diffuse_factor=.3,temporal=dict(open=not a.fresh,history_limit=5,radius=0,theta=35,depth=.3),spatial=dict(open=not a.fresh,sample_num=2,radius=12,theta=35,depth=.3)))
env=dict(VISION_RESTIR_GI_MULTIBOUNCE_BENCH='1',VISION_RESTIR_GI_NATIVE_ALIGNED_LAMBERT='1',VISION_BENCH_SAMPLE_OFFSET=str(a.offset),VISION_RESTIR_GI_NATIVE_MB_DIAGNOSTIC='1' if a.phase=='pilot' else '0')
c['benchmark_environment']=env
original=runner.engine_environment
def environment(source,cache,port):
    clean={k:v for k,v in source.items() if not k.startswith(('VISION_RESTIR_','VISION_PT_','VISION_BENCH_','VISION_FALCOR_','CORONA_DI_'))}
    value=original(clean,cache,port);value.update(env);return value
with patch.object(runner,'engine_environment',environment):result=runner.run(c)
log=(out/'engine.log').read_text(encoding='utf-8',errors='replace')
expected='Native ReSTIR GI multibounce benchmark:'
ok=expected in log and 'RestirGI reference route active:' not in log and 'Native ReSTIR GI material contract: separate=false, suffix_depth=15' in log
(out/'route-validation.json').write_text(json.dumps(dict(expected_log=expected,observed=ok,port_route_absent='RestirGI reference route active:' not in log),indent=2))
print(json.dumps(dict(output=str(out),status=result.get('status'),route=ok,error=result.get('error')),indent=2),flush=True)
raise SystemExit(0 if ok and result.get('status')=='complete' else 1)
