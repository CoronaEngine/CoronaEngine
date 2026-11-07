"""Freeze shared Kitchen and bake equivalent world-space OBJ instances for Corona."""
from pathlib import Path
import hashlib
import importlib.util
import json
import math
import shutil
import sys
import xml.etree.ElementTree as ET
import numpy as np

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parent
OLD=ROOT.parent/'rtxgi-kitchen-bench-20261107'
SHARED=ROOT/'shared-scene';SHARED.mkdir(exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
for name in ['kitchen-gray.gltf','kitchen.bin']:
    dest=SHARED/name
    if not dest.exists():shutil.copy2(OLD/'shared-scene'/name,dest)
    assert sha(dest)==sha(OLD/'shared-scene'/name)
model=SHARED/'kitchen-gray.gltf'
(SHARED/'kitchen.scene.json').write_text(json.dumps(dict(models=[str(model).replace('\\','/')],graph=[dict(name='SharedKitchen',model=0)]),indent=2))
spec=importlib.util.spec_from_file_location('oracle',OLD/'cpu_shadow_oracle.py')
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
triangles,ranges=oracle.load_triangles(model)
triangles=triangles.astype(np.float32)
MESH=SHARED/'world-meshes';MESH.mkdir(exist_ok=True)
records=[]
for idx,r in enumerate(ranges):
    part=triangles[r['first']:r['first']+r['count']]
    vertex,inverse=np.unique(part.reshape(-1,3),axis=0,return_inverse=True)
    p=MESH/f'mesh_{idx:03d}.obj'
    with p.open('w',encoding='ascii',newline='\n') as f:
        f.write('# Shared Kitchen world coordinates; geometric normals selected by benchmark.\n')
        np.savetxt(f,vertex,fmt='v %.9g %.9g %.9g')
        np.savetxt(f,inverse.reshape(-1,3)+1,fmt='f %d %d %d')
    records.append(dict(**r,path=str(p),sha256=sha(p),vertices=len(vertex),triangles=len(part)))
scene=ET.Element('scene',version='3.0.0')
def add(parent,tag,**attrs):return ET.SubElement(parent,tag,{k:str(v) for k,v in attrs.items()})
sensor=add(scene,'sensor',type='perspective')
for name,value in [('fov',60),('near_clip',.01),('far_clip',1000)]:add(sensor,'float',name=name,value=value)
add(sensor,'string',name='fov_axis',value='y')
forward=np.array([-.4381,-.0416703,-.89796]);forward/=np.linalg.norm(forward)
position=np.array([1.211005,1.804751,3.852390]);target=position+forward
xf=add(sensor,'transform',name='to_world')
add(xf,'lookat',origin=','.join(map(str,position)),target=','.join(map(str,target)),up='0,1,0')
film=add(sensor,'film',type='hdrfilm')
add(film,'integer',name='width',value=1920);add(film,'integer',name='height',value=1080)
bs=add(scene,'bsdf',type='twosided',id='gray')
add(add(bs,'bsdf',type='diffuse'),'rgb',name='reflectance',value='.7,.7,.7')
for idx,r in enumerate(records):
    s=add(scene,'shape',type='obj',id=f'world_{idx:03d}')
    add(s,'string',name='filename',value=Path(r['path']).as_posix())
    add(s,'boolean',name='flip_tex_coords',value='false');add(s,'ref',id='gray')
add(add(scene,'emitter',type='constant'),'rgb',name='radiance',value='0,0,0')
sun=add(scene,'emitter',type='directional')
add(sun,'vector',name='direction',value='0,-0.4472135954999579,0.8944271909999159')
add(sun,'rgb',name='irradiance',value='1,1,1')
integrator=add(scene,'integrator',type='path')
add(integrator,'integer',name='max_depth',value=16);add(integrator,'integer',name='min_depth',value=5)
add(integrator,'float',name='rr_threshold',value=1)
ET.indent(scene,space='  ')
xml=SHARED/'kitchen-world.xml';ET.ElementTree(scene).write(xml,encoding='utf-8',xml_declaration=True)
repo=ROOT.parents[2]
sys.path.insert(0,str(repo/'CoronaEditor/editor'))
from runtime.archive.external_scene import parse_external_scene
snapshot=parse_external_scene(xml,cache_directory=repo/'.workspace/tmp/rtxgi-multibounce-20261107/parser-cache')
assert len(snapshot['scene']['actors'])==len(records)
camera=snapshot['scene']['cameras'][0]
assert np.allclose(camera['position'],position) and np.allclose(camera['forward'],forward)
assert math.isclose(camera['fov'],60,abs_tol=1e-8) and camera['projection_x_sign']==-1
contract=json.loads((OLD/'scene_contract.json').read_text())
contract.update(schema='four-way-restirgi-multibounce-v1',model=str(model),corona_scene=str(xml),
                scene_sha256=sha(xml),geometry_meshes=len(records),geometry_triangles=len(triangles),
                geometry_world_float32_triangle_sha256=hashlib.sha256(triangles.tobytes()).hexdigest(),
                camera=dict(contract['camera'],corona_api={k:camera[k] for k in ['position','forward','world_up','fov','projection_x_sign','near_plane','far_plane']}),
                transport=dict(min_depth=5,max_depth=16,rr_threshold=1,depth_count='camera0 primary1; max16 counts primary',
                    indirect_NEE_surface_depths=list(range(2,17)),RR_first_after_surface=6,
                    roulette='q=min(.95,beta_camera_before_current_BSDF) if beta<1, then postscatterBeta/=q on success; no.05floor',
                    primary_direct_in_AOV=False,environment_and_emission=False,
                    candidate_radiance='full outgoing suffix integral at first secondary',initial_candidate_paths_per_pixel=1),
                reference=dict(mode='unpacked independent full PT paths; min5 max16 RR1',samples_per_half=1024,split_halves=2,
                               offsets=[100000,200000],common_reference_requires_agreement=True),
                original_native_reuse=dict(primary_proposal='uniform hemisphere, retained',temporal_M_clamp=20,W_clamp=10,
                                           spatial_passes=2,interleaved_short_paths=False),
                renderers=['originalRestirGI','RTXPT','RTXDI','CoronaRestirGIport'],
                renderer_coordinates=dict(canonical='glTF world',Corona_internal_from_canonical=[1,1,-1],
                    Corona_internal_to_sun=[0,1/math.sqrt(5),2/math.sqrt(5)],
                    guide_comparison='reflect Corona position/normal Z back before matching; same physical light/camera/geometry'),
                assumptions='Equal1candidatepath/frame, commontransport/RR; native reuse algorithms and proposal differences retained, actualpathdepth/costreported.',
                files={n:dict(sha256=sha(SHARED/n),bytes=(SHARED/n).stat().st_size) for n in ['kitchen-gray.gltf','kitchen.bin','kitchen.scene.json','kitchen-world.xml']})
(ROOT/'scene_contract.json').write_text(json.dumps(contract,indent=2),encoding='utf-8')
(ROOT/'world-mesh-manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
(ROOT/'corona-parser-snapshot.json').write_text(json.dumps(snapshot,indent=2),encoding='utf-8')
print(json.dumps(dict(model=str(model),corona_scene=str(xml),meshes=len(records),triangles=len(triangles),camera=camera,contract=str(ROOT/'scene_contract.json')),indent=2))
