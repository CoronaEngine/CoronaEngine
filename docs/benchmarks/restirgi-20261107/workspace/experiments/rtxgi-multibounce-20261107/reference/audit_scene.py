"""CPU-only Assimp import through the exact original application's runtime DLL."""
from pathlib import Path
import ctypes as c
import hashlib
import json
import numpy as np
import sys
import importlib.util

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
COPY=ROOT/'.workspace/cache/rtxgi-multibounce-20261107/reference'
contract=json.loads((HERE.parent/'scene_contract.json').read_text())
class Vec(c.Structure): _fields_=[('x',c.c_float),('y',c.c_float),('z',c.c_float)]
class Face(c.Structure): _fields_=[('count',c.c_uint),('indices',c.POINTER(c.c_uint))]
class Mesh(c.Structure):
    _fields_=[('primitive',c.c_uint),('vertices_count',c.c_uint),('faces_count',c.c_uint),
        ('vertices',c.POINTER(Vec)),('normals',c.c_void_p),('tangents',c.c_void_p),('bitangents',c.c_void_p),
        ('colors',c.c_void_p*8),('texcoords',c.c_void_p*8),('uvcounts',c.c_uint*8),('faces',c.POINTER(Face))]
class Scene(c.Structure):
    _fields_=[('flags',c.c_uint),('root',c.c_void_p),('meshes_count',c.c_uint),('meshes',c.POINTER(c.POINTER(Mesh)))]
dll=c.CDLL(str(COPY/'bin-release/assimp-vc140-mt.dll'))
dll.aiGetVersionMajor.restype=c.c_uint;dll.aiGetVersionMinor.restype=c.c_uint
dll.aiImportFile.argtypes=[c.c_char_p,c.c_uint];dll.aiImportFile.restype=c.POINTER(Scene)
dll.aiGetErrorString.restype=c.c_char_p;dll.aiReleaseImport.argtypes=[c.POINTER(Scene)]
path=Path(sys.argv[1]) if len(sys.argv)>1 else Path(contract['model'])
pointer=dll.aiImportFile(str(path).replace('\\','/').encode(),0x2|0x8)
result={'GPU_launched':False,'assimp_version':[dll.aiGetVersionMajor(),dll.aiGetVersionMinor()],
    'input':str(path),'flags':'JoinIdenticalVertices|Triangulate; canonical world OBJ, no axis conversion',
    'import_succeeded':bool(pointer)}
if pointer and pointer.contents.root and pointer.contents.meshes_count:
    spec=importlib.util.spec_from_file_location('shadow_oracle',HERE.parent.parent/'rtxgi-kitchen-bench-20261107/cpu_shadow_oracle.py')
    oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
    canonical,_=oracle.load_triangles(Path(contract['model']));canonical=canonical.astype('<f4')
    scene=pointer.contents;sha=hashlib.sha256();triangles=0;mins=[];maxs=[];max_delta=0.0;different=0
    for i in range(scene.meshes_count):
        m=scene.meshes[i].contents
        vertices=np.ctypeslib.as_array(c.cast(m.vertices,c.POINTER(c.c_float)),shape=(m.vertices_count*3,)).reshape(-1,3)
        faces=np.empty((m.faces_count,3),dtype=np.int64)
        for j in range(m.faces_count):
            face=m.faces[j];assert face.count==3
            faces[j]=np.ctypeslib.as_array(face.indices,shape=(3,))
        world=vertices[faces].astype('<f4');sha.update(world.tobytes());triangles+=m.faces_count
        target=canonical[triangles-m.faces_count:triangles]
        max_delta=max(max_delta,float(np.max(np.abs(world-target))))
        different+=int(np.count_nonzero(world.view(np.uint32)!=target.view(np.uint32)))
        mins.append(vertices.min(0));maxs.append(vertices.max(0))
    result.update(meshes=scene.meshes_count,triangles=triangles,
        world_float32_triangle_sha256=sha.hexdigest(),bounds=[np.min(mins,0).tolist(),np.max(maxs,0).tolist()])
    result.update(canonical_triangle_sha256=hashlib.sha256(canonical.tobytes()).hexdigest(),
        maximum_absolute_coordinate_delta=max_delta,coordinate_bit_differences=different,
        triangle_order_equal_within_1micrometer=bool(max_delta<1e-6))
    dll.aiReleaseImport(pointer)
else:
    result['import_succeeded']=False
    result['error']=dll.aiGetErrorString().decode(errors='replace')
    if pointer:result['scene_flags']=pointer.contents.flags
(HERE/'scene-import-audit.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
if not result['import_succeeded']:raise SystemExit(1)
