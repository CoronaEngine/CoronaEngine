"""Build only: original ReSTIR GI multibounce benchmark isolated copy."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
COPY=ROOT/'.workspace/cache/rtxgi-multibounce-20261107/reference'
SRC=COPY/'RestirGI'
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    command=['C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe',
        str(SRC/'RestirGI.vcxproj'),'/p:Configuration=Release','/p:Platform=x64',
        '/p:PlatformToolset=v143','/p:VCToolsVersion=14.38.33130',
        '/p:WindowsTargetPlatformVersion=10.0.22621.0','/m:1','/nologo','/v:minimal']
    env=os.environ.copy();env['_CL_']='/MP3 /utf-8 /wd4819 /wd4828'
    start=time.time()
    with (HERE/'build.log').open('w',encoding='utf-8') as log:
        result=subprocess.run(command,cwd=COPY,env=env,stdout=log,stderr=subprocess.STDOUT)
    record={'command':command,'exit_code':result.returncode,'seconds':time.time()-start,'GPU_launched':False}
    (HERE/'build-result.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(record),flush=True)
    if result.returncode: raise SystemExit(result.returncode)
    probe=COPY/'bin-release/cpu_shader_probe.exe'
    shaders={
        'GbufferRchit.hlsl':[('lib_6_3','')], 'GbufferRMiss.hlsl':[('lib_6_3','')],
        'RtdgiTraceRgen.hlsl':[('lib_6_3','')], 'RtdgiValidateRgen.hlsl':[('lib_6_3','')],
        'TraceSunShadowMaskRgen.hlsl':[('lib_6_3','')],
        'RasterGbufferPass.hlsl':[('ps_6_0','PSMain'),('vs_6_0','VSMain')],
        'restirTemporalCS.hlsl':[('cs_6_3','CS')], 'restirSpatialCS.hlsl':[('cs_6_3','CS')],
        'restirResolveCS.hlsl':[('cs_6_3','CS')], 'copyTestCS.hlsl':[('cs_6_3','CS')],
        'extractGiViewNormalCS.hlsl':[('cs_6_3','CS')], 'lightGbufferCS.hlsl':[('cs_6_3','CS')],
    }
    records=[]
    for name, stages in shaders.items():
        for target,entry in stages:
            args=[str(probe),'shaders/'+name,target,entry,'2018']
            r=subprocess.run(args,cwd=SRC,capture_output=True)
            log=HERE/f'shader-{name}-{target}.log';log.write_bytes(r.stdout+r.stderr)
            records.append({'source':name,'target':target,'entry':entry,'exit_code':r.returncode,'log':str(log)})
            print(name,target,r.returncode,flush=True)
    (HERE/'shader-compile.json').write_text(json.dumps({'GPU_launched':False,'compiles':records},indent=2))
    if any(r['exit_code'] for r in records): raise SystemExit(1)
    shutil.copytree(SRC/'shaders',COPY/'bin-release/shaders',dirs_exist_ok=True)
    for name in ['Structure.h','ShaderInterop.h']:shutil.copy2(SRC/name,COPY/'bin-release'/name)
    native=json.loads((HERE/'native-core-hashes.json').read_text())
    for name,expected in native.items(): assert digest(SRC/'shaders'/name)==expected,name
    files=sorted(p for p in SRC.rglob('*') if p.is_file() and p.suffix.lower() in ['.cpp','.h','.hlsl','.vcxproj'])
    manifest={'GPU_launched':False,'executable':str(COPY/'bin-release/RestirGI.exe'),
        'executable_sha256':digest(COPY/'bin-release/RestirGI.exe'),
        'native_core_hashes':native,
        'source_sha256':{str(p.relative_to(SRC)):digest(p) for p in files}}
    (HERE/'build-manifest.json').write_text(json.dumps(manifest,indent=2))

if __name__=='__main__':main()
