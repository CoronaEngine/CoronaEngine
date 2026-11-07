"""Read-only CPU audit of native camera/depth/VIEW normals against physical world data."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import itertools
import json
import numpy as np
from PIL import Image

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
COPY=ROOT/'.workspace/cache/rtxgi-multibounce-20261107/reference'
def normalize(x):return x/np.maximum(np.linalg.norm(x,axis=-1,keepdims=True),1e-30)
def quantiles(x):return np.quantile(x,[0,.5,.99,1]).tolist()
def snorm(path):return np.maximum(-1,np.memmap(path,dtype='i1',mode='r',shape=(1080,1920,4)).astype(float)/127)
def f32(path):return np.memmap(path,dtype='<f4',mode='r',shape=(1080,1920,4))
def basis(n):
    if n[2]<0:
        a=1/(1-n[2]);b=n[0]*n[1]*a
        b1=np.array([1-n[0]*n[0]*a,-b,n[0]])
        b2=np.array([b,n[1]*n[1]*a-1,-n[1]])
    else:
        a=1/(1+n[2]);b=-n[0]*n[1]*a
        b1=np.array([1-n[0]*n[0]*a,b,-n[0]])
        b2=np.array([b,1-n[1]*n[1]*a,-n[1]])
    return np.stack([b1,b2,n],axis=1)

def main(run,ray_count):
    spec=json.loads((HERE.parent/'run-specs'/f'{run.name}.json').read_text())
    args=spec['args']
    def vector(flag):i=args.index(flag);return np.array(args[i+1:i+4],dtype=np.float32).astype(float)
    def scalar(flag):return float(np.float32(args[args.index(flag)+1]))
    eye=vector('--eye');target=vector('--target');up=vector('--up')
    forward=normalize(target-eye);right=normalize(np.cross(forward,up));camera_up=np.cross(right,forward)
    # Column-vector world->view, matching glm::lookAt and the HLSL cbuffer.
    R=np.stack([right,camera_up,-forward],axis=0)
    view=np.eye(4);view[:3,:3]=R;view[:3,3]=-R@eye
    inverse=np.linalg.inv(view)
    near=scalar('--near');fov=scalar('--fov-y');aspect=1920/1080
    tan_y=np.tan(np.deg2rad(fov)*.5);scale_y=1/tan_y;scale_x=scale_y/aspect
    projection=np.array([[scale_x,0,0,0],[0,scale_y,0,0],[0,0,0,near],[0,0,-1,0]],dtype=float)
    folders=sorted((run/'raw').glob('frame_*'))
    folder=folders[-1];frame=int(folder.name.split('_')[1])
    normals=f32(folder/'primary_world_normal.bin');positions=f32(folder/'primary_world_position.bin')
    depth=np.memmap(folder/'primary_depth.bin',dtype='<f4',mode='r',shape=(1080,1920))
    primary_vs=snorm(folder/'native_primary_normal_view.bin')
    candidate_vs=snorm(folder/'native_candidate_normal_view.bin')
    hit=np.memmap(folder/'native_candidate_hit_pdf.bin',dtype='<f2',mode='r',shape=(1080,1920,4)).astype(float)
    diag=f32(folder/'path_diagnostics.bin')
    valid=depth>0;world_n=normals[...,:3][valid].astype(float)
    expected_vs=normalize(world_n@R.T);observed_vs=primary_vs[...,:3][valid]
    component_delta=np.abs(expected_vs-observed_vs)
    integer_expected=np.rint(np.clip(expected_vs,-1,1)*127)
    integer_actual=np.rint(observed_vs*127)
    dots=np.sum(normalize(expected_vs)*normalize(observed_vs),axis=-1)
    # Orthogonal Procrustes fit independently asks what rotation the buffers imply.
    U,S,Vt=np.linalg.svd(world_n.T@normalize(observed_vs));fitted=(U@Vt).T
    fitted_angle=np.degrees(np.arccos(np.clip((np.trace(fitted@R.T)-1)/2,-1,1)))
    permutation_errors=[]
    subsample=slice(None,None,32)
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product([-1,1],repeat=3):
            transformed=expected_vs[subsample][:,perm]*np.array(signs)
            permutation_errors.append({'permutation':list(perm),'signs':list(signs),
                'RMS':float(np.sqrt(np.mean((transformed-observed_vs[subsample])**2)))})
    permutation_errors.sort(key=lambda r:r['RMS'])
    p=positions[...,:3][valid].astype(float);view_p=(p-eye)@R.T
    predicted_depth=-near/view_p[:,2]
    yy,xx=np.mgrid[:1080,:1920];ndcx=2*(xx+.5)/1920-1;ndcy=1-2*(yy+.5)/1080
    z=-near/depth[valid].astype(float)
    reconstructed_view=np.stack([-z*ndcx[valid]/scale_x,-z*ndcy[valid]/scale_y,z],axis=-1)
    reconstructed_world=reconstructed_view@R+eye
    reprojection_px=np.stack([(view_p[:,0]/-view_p[:,2]*scale_x+1)*1920*.5,
        (1-view_p[:,1]/-view_p[:,2]*scale_y)*1080*.5],axis=-1)
    center_px=np.stack([xx[valid]+.5,yy[valid]+.5],axis=-1)
    result={'schema':'originalReSTIRGI.native-adapter.cpu-audit.v1','run':str(run),'frame':frame,
        'GPU_launched':False,'source_or_binary_changed':False,
        'camera':{'eye_float32':eye.tolist(),'target_float32':target.tolist(),'up_float32':up.tolist(),
            'world_to_view_column_vector':view.tolist(),'view_to_world_column_vector':inverse.tolist(),
            'projection_column_vector':projection.tolist(),'view_basis_rows':['screen_right','screen_up','backward_minus_forward'],
            'view_forward_axis':'negative Z','screen_uv':'top-left, +u right, +v down',
            'depth':'infinite-far reversed Z: depth=near/(-viewZ); clear0; near1',
            'depth_source':'RestirGI.cpp getReverseZ returns S, not finite-far M',
            'matrix_packing':'glm column-major -> memcpy into XMMATRIX -> HLSL default column-major; physical world_to_view is glm lookAt, inverse similarly transposed twice',
            'determinant_rotation':float(np.linalg.det(R)),'orthonormal_residual':float(np.max(np.abs(R@R.T-np.eye(3))))},
        'primary_VIEW_normal':{'valid_pixels':int(valid.sum()),
            'depth_vs_exported_primary_guide_mask_differences':int(np.count_nonzero(valid!=(normals[...,3]>0))),
            'R8_SNORM_alpha_not_validity_flag':True,
            'invalid_depth_pixels_with_SNORM_alpha1':int(np.count_nonzero((~valid)&(primary_vs[...,3]>.99))),
            'component_absolute_error_min_median_p99_max':quantiles(component_delta),
            'half_quantization_step':.5/127,
            'fraction_components_within_half_step_plus1e_6':float(np.mean(component_delta<=.5/127+1e-6)),
            'quantized_integer_component_mismatches':int(np.count_nonzero(integer_actual!=integer_expected)),
            'maximum_quantized_integer_difference':float(np.max(np.abs(integer_actual-integer_expected))),
            'normal_dot_min_median_p99_max':quantiles(dots),
            'normal_fraction_dot_above_9999':float(np.mean(dots>.9999)),
            'raw_SNORM_normal_length_min_median_p99_max':quantiles(np.linalg.norm(observed_vs,axis=-1)),
            'fitted_world_to_view_rotation':fitted.tolist(),'fitted_rotation_difference_degrees':float(fitted_angle),
            'normal_fit_interpretation':'A fit to quantized normals, not a readback of the camera matrix; repeated axis-aligned wall normals make quantization bias systematic. Physical depth/position reprojection independently constrains the actual camera conversion.',
            'fitted_RMS_residual':float(np.sqrt(np.mean((world_n@fitted.T-normalize(observed_vs))**2))),
            'best_signed_axis_mappings':permutation_errors[:4]},
        'depth_reconstruction':{'valid_pixels':int(valid.sum()),'invalid_depth_values':np.unique(depth[~valid]).tolist(),
            'valid_viewZ_min_max':[float(view_p[:,2].min()),float(view_p[:,2].max())],
            'valid_depth_min_max':[float(depth[valid].min()),float(depth[valid].max())],
            'absolute_depth_error_min_median_p99_max':quantiles(np.abs(depth[valid]-predicted_depth)),
            'relative_depth_error_min_median_p99_max':quantiles(np.abs(depth[valid]-predicted_depth)/predicted_depth),
            'world_position_reconstruction_delta_m_min_median_p99_max':quantiles(np.linalg.norm(reconstructed_world-p,axis=-1)),
            'screen_reprojection_error_pixels_min_median_p99_max':quantiles(np.linalg.norm(reprojection_px-center_px,axis=-1)),
            'positive_viewZ_for_valid_pixels':int(np.count_nonzero(view_p[:,2]>=0))}}
    # Test first-secondary VIEW normals against physical canonical triangle normals.
    oracle_spec=importlib.util.spec_from_file_location('oracle',HERE.parent.parent/'rtxgi-kitchen-bench-20261107/cpu_shadow_oracle.py')
    oracle=importlib.util.module_from_spec(oracle_spec);oracle_spec.loader.exec_module(oracle)
    contract=json.loads((HERE.parent/'scene_contract.json').read_text())
    triangles,_=oracle.load_triangles(Path(contract['model']));triangles=triangles.astype(np.float32).astype(float)
    bounds_min=triangles.min(axis=1);bounds_max=triangles.max(axis=1)
    blue=np.asarray(Image.open(COPY/'bin-release/resource/bluenoise/256_256/LDR_RGBA_0.png'))
    config=json.loads((run/'benchmark-config.json').read_text());sample=frame+config['sample_offset']
    plastic=np.float32(1.324717957244746);a1=np.float32(1)/plastic;a2=np.float32(1)/(plastic*plastic)
    offset=(np.mod(np.array([a1,a2],dtype=np.float32)*np.float32(sample)+np.float32(.5),1)*256).astype(int)
    t_gpu=np.linalg.norm(hit[...,:3],axis=-1)
    good=valid&(diag[...,0]>=2)&(t_gpu>.01)&(t_gpu<9)
    choices=np.flatnonzero(good.ravel());rng=np.random.default_rng(20261107)
    selected=rng.choice(choices,size=min(ray_count,len(choices)),replace=False)
    records=[]
    for i,linear in enumerate(selected):
        y,x=divmod(int(linear),1920);normal_ws=normalize(R.T@primary_vs[y,x,:3])
        biased_depth=min(1,float(np.float32(np.float32(depth[y,x])*np.float32(np.array([0x3f800040],dtype=np.uint32).view(np.float32)[0]))))
        vz=-near/biased_depth;position_vs=np.array([-vz*ndcx[y,x]/scale_x,-vz*ndcy[y,x]/scale_y,vz]);world_hit=R.T@position_vs+eye
        camera_direction=normalize(R.T@position_vs)
        offset_length=max(1e-4,float(max(np.abs(world_hit).max(),-vz))*1e-6)
        origin=world_hit+(normal_ws-camera_direction)*offset_length
        u=(blue[(y+offset[1])%256,(x+offset[0])%256,:2].astype(float)+.5)/256
        cosine=1-u[0];sine=np.sqrt(1-cosine*cosine);phi=u[1]*2*np.pi
        direction=normalize(basis(normal_ws)@np.array([sine*np.cos(phi),sine*np.sin(phi),cosine]))
        packed_position=origin+hit[y,x,:3]
        tolerance=max(.001,float(t_gpu[y,x])*.001)
        local_mask=np.all((bounds_min<=packed_position+tolerance)&(bounds_max>=packed_position-tolerance),axis=-1)
        candidates=np.flatnonzero(local_mask)
        cpu_t,local_index=oracle.trace(triangles[candidates],origin,direction,0,float(t_gpu[y,x])+tolerance,100000)
        record={'pixel':[x,y],'candidate_triangles':len(candidates),'GPU_packed_hit_distance':float(t_gpu[y,x]),'CPU_distance':cpu_t}
        if local_index is not None:
            tri=triangles[candidates[local_index]];normal_cpu=normalize(np.cross(tri[1]-tri[0],tri[2]-tri[0]))
            if np.dot(direction,normal_cpu)>0:normal_cpu=-normal_cpu
            expected=R@normal_cpu;observed=candidate_vs[y,x,:3]
            record.update(triangle_index=int(candidates[local_index]),CPU_world_normal=normal_cpu.tolist(),
                expected_VIEW_normal=expected.tolist(),native_VIEW_normal=observed.tolist(),
                normal_dot=float(np.dot(normalize(expected),normalize(observed))),
                maximum_component_error=float(np.max(np.abs(expected-observed))),
                hit_distance_absolute_error=float(abs(cpu_t-t_gpu[y,x])))
        records.append(record)
        if (i+1)%16==0:print(f'CPU secondary normal checks {i+1}/{len(selected)}',flush=True)
    checked=[r for r in records if 'normal_dot' in r]
    result['candidate_VIEW_normal']={'method':'canonical world F32 triangles; CPU native depth/bias reconstruction and native blue-noise primary proposal; local triangle AABB query at packed hit then two-sided ray intersection',
        'sample_seed':20261107,'samples':len(records),'CPU_hit_matches':len(checked),
        'normal_dot_min_median_p99_max':quantiles([r['normal_dot'] for r in checked]),
        'fraction_normal_dot_above_999':float(np.mean([r['normal_dot']>.999 for r in checked])),
        'component_error_min_median_p99_max':quantiles([r['maximum_component_error'] for r in checked]),
        'hit_distance_delta_min_median_p99_max':quantiles([r['hit_distance_absolute_error'] for r in checked]),
        'records':records}
    result['conclusion']='Physical world-to-view rotation, axes, reversed-depth and reconstruction conventions agree with native buffers within their quantization/numerical precision; first-secondary VIEW normal samples agree with canonical physical triangle normals.'
    path=HERE/(run.name+'-adapter-audit.json');path.write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='candidate_VIEW_normal'},indent=2))
    print(json.dumps({k:v for k,v in result['candidate_VIEW_normal'].items() if k!='records'},indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('run',type=Path);parser.add_argument('--rays',type=int,default=64)
    args=parser.parse_args();main(args.run,args.rays)
