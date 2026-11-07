"""CPU evidence: native packed candidate estimator versus the exact same traced path."""
from pathlib import Path
import argparse
import json
import numpy as np
from PIL import Image

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
ASSETS=ROOT/'.workspace/cache/rtxgi-multibounce-20261107/reference/bin-release/resource'
def norm(v):return v/np.maximum(np.linalg.norm(v,axis=-1,keepdims=True),1e-30)
def main(run):
    contract=json.loads((HERE.parent/'scene_contract.json').read_text())
    forward=norm(np.array(contract['camera']['forward']))
    right=norm(np.cross(forward,np.array(contract['camera']['up'])))
    camera_up=np.cross(right,forward)
    view_to_world=np.stack([right,camera_up,-forward],axis=1)
    blue=np.asarray(Image.open(ASSETS/'bluenoise/256_256/LDR_RGBA_0.png'))
    config=json.loads((run/'benchmark-config.json').read_text())
    yy,xx=np.mgrid[:1080,:1920];records=[]
    for folder in sorted((run/'raw').glob('frame_*')):
        frame=int(folder.name.split('_')[1]);sample=frame+config['sample_offset']
        def f(name,dtype='<f4'):return np.memmap(folder/(name+'.bin'),dtype=dtype,mode='r',shape=(1080,1920,4)).astype(np.float64)
        lo=f('native_candidate_radiance','<f2');hit=f('native_candidate_hit_pdf','<f2')
        primary_vs=f('native_primary_normal_view','i1')[...,:3]/127
        valid=(hit[...,3]>0)&np.isfinite(hit[...,3])
        candidate=np.zeros_like(lo[...,:3]);candidate[valid]=lo[...,:3][valid]*(.7/np.pi)/hit[...,3][valid,None]
        exact=f('fresh_pt')[...,:3];native=f('raw_gi')[...,:3]
        normal=norm(primary_vs@view_to_world.T);direction=norm(hit[...,:3])
        n=normal;minus=n[...,2]<0;denom=1+np.where(minus,-n[...,2],n[...,2]);a=1/denom
        cross=np.where(minus,n[...,0]*n[...,1]*a,-n[...,0]*n[...,1]*a)
        b1=np.stack([1-n[...,0]*n[...,0]*a,np.where(minus,-cross,cross),np.where(minus,n[...,0],-n[...,0])],axis=-1)
        b2=np.stack([cross,np.where(minus,n[...,1]*n[...,1]*a-1,1-n[...,1]*n[...,1]*a),-n[...,1]],axis=-1)
        local=np.stack([np.sum(direction*b1,axis=-1),np.sum(direction*b2,axis=-1),np.sum(direction*n,axis=-1)],axis=-1)
        phi=np.mod(np.arctan2(local[...,1],local[...,0])/(2*np.pi)+1,1)
        recovered=np.clip(np.floor(np.stack([1-local[...,2],phi],axis=-1)*256),0,255).astype(int)
        plastic=np.float32(1.324717957244746)
        a1=np.float32(1)/plastic;a2=np.float32(1)/(plastic*plastic)
        offset=(np.mod(np.array([a1,a2],dtype=np.float32)*np.float32(sample)+np.float32(.5),np.float32(1))*256).astype(int)
        expected=blue[(yy+offset[1])%256,(xx+offset[0])%256,:2]
        same=np.all(recovered==expected,axis=-1)
        cosine_from_pdf=np.zeros_like(hit[...,3]);cosine_from_pdf[valid]=1/(2*np.pi*hit[...,3][valid])
        cosine_geometry=np.maximum(0,np.sum(normal*direction,axis=-1))
        records.append({'frame':frame,'candidate_mean_rgb':candidate.mean((0,1)).tolist(),
            'same_unpacked_candidate_mean_rgb':exact.mean((0,1)).tolist(),'native_GI_mean_rgb':native.mean((0,1)).tolist(),
            'candidate_vs_unpacked_relative_mean':float(candidate.mean()/exact.mean()-1),
            'recovered_proposal_8bit_matches_GPU_source_fraction':float(same[valid].mean()),
            'recovered_proposal_mismatches':int(np.count_nonzero(valid&~same)),
            'cosine_from_packed_pdf_vs_primary_direction_max':float(np.max(np.abs(cosine_from_pdf[valid]-cosine_geometry[valid]))),
            'all_candidate_finite':bool(np.isfinite(candidate).all()),
            'first_hit_offset_length_p1_p50_p99':np.quantile(np.linalg.norm(hit[valid,:3],axis=-1),[.01,.5,.99]).tolist()})
    data={'run':str(run),'estimator':'nativePackedLo*(rho/pi)/nativePackedPdf; same pre-reuse path as fresh_pt native-mode output','frames':records}
    out=HERE/(run.name+'-candidate-audit.json')
    out.write_text(json.dumps(data,indent=2));print(json.dumps(data,indent=2))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('run',type=Path);args=parser.parse_args();main(args.run)
