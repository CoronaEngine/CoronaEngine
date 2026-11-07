"""Baked shared world triangles for the original Assimp3.3 OBJ reader."""
from pathlib import Path
import hashlib
import json
import numpy as np

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
ASSET=ROOT/'.workspace/cache/rtxgi-multibounce-20261107/reference/assets/kitchen-world.obj'
ASSET.parent.mkdir(exist_ok=True)
records=json.loads((HERE.parent/'world-mesh-manifest.json').read_text())
offset=0
with ASSET.open('w',encoding='ascii',newline='\n') as output:
    output.write('# Canonical shared Kitchen world geometry; no axis reflection.\n')
    for i,record in enumerate(records):
        source=Path(record['path'])
        assert hashlib.sha256(source.read_bytes()).hexdigest()==record['sha256']
        output.write(f'o shared_world_{i:03d}\n')
        for line in source.read_text().splitlines():
            if line.startswith('v '):output.write(line+'\n')
            elif line.startswith('f '):output.write('f '+' '.join(str(int(x)+offset) for x in line.split()[1:])+'\n')
        offset+=record['vertices']
manifest={'GPU_launched':False,'asset':str(ASSET),'asset_sha256':hashlib.sha256(ASSET.read_bytes()).hexdigest(),
    'world_triangles':sum(x['triangles'] for x in records),'source_world_meshes':len(records),
    'reason':'Original shipped Assimp3.3 cannot load glTF2; OBJ uses canonical shared world float32 triangles.'}
(HERE/'asset-manifest.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(manifest,indent=2))
