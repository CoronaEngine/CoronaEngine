"""CPU-only exact-frame timing summaries for the four frozen renderer formats.

python -B analyze_timing.py manifest.json --output timing.json
Uses manifest timing entries, without loading raw radiance. No app/GPU actions.
"""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import math

import numpy as np
import analyze_multibounce as analysis

PROFILES = {
    'rtxpt': {'frame':'frame','name':'name','ms':'ms',
              'gi':['GI.Candidate','GI.Temporal','GI.Spatial','GI.Final'],
              'prep':['SP build','SP export'],'full':'Frame.RenderGPU'},
    'rtxdi': {'frame':'render_frame','name':'section','ms':'gpu_ms',
              'gi':['BRDF or MIS Rays','Shade Secondary Surf.','GI - Temporal Resampling','GI - Spatial Resampling','GI - Final Shading'],
              'prep':['G-Buffer Fill'],'full':'Frame Time (GPU)'},
    'original': {'frame':'frame','name':'stage','ms':'ms',
                 'gi':['rtdgiTraceRT','rtdgiValidateRT','restirTemporalCS','restirSpatialCS','restirResolveCS'],
                 'prep':['primaryGBuffer','reprojectionMapCS','copyDepthCS','extractGiViewNormalCS','validityIntegrateCS'],
                 'full':None},
    'corona': {'frame':'frame','name':'name','ms':'ms',
               'gi':['RestirGI reference candidates','RestirGI reference temporal reuse',
                     'RestirGI reference spatial reuse 0','RestirGI reference spatial reuse 1','RestirGI reference resolve'],
               'prep':['rt_geom','RestirGI reference surface preparation','FrameBuffer-SaveSurfaceExtHistory'],'full':None},
}
CORONA_VALIDATION='RestirGI reference validation'
STAGE_GROUPS={
    'original':{'candidate':['rtdgiTraceRT'],'validation':['rtdgiValidateRT'],'temporal':['restirTemporalCS'],
                'spatial':['restirSpatialCS'],'resolve':['restirResolveCS']},
    'rtxpt':{'candidate':['GI.Candidate'],'validation':[],'temporal':['GI.Temporal'],'spatial':['GI.Spatial'],'resolve':['GI.Final']},
    'rtxdi':{'candidate':['BRDF or MIS Rays','Shade Secondary Surf.'],'validation':[],
             'temporal':['GI - Temporal Resampling'],'spatial':['GI - Spatial Resampling'],'resolve':['GI - Final Shading']},
    'corona':{'candidate':['RestirGI reference candidates'],'validation':[CORONA_VALIDATION],
              'temporal':['RestirGI reference temporal reuse'],
              'spatial':['RestirGI reference spatial reuse 0','RestirGI reference spatial reuse 1'],
              'resolve':['RestirGI reference resolve']},
}


def describe(values):
    x=np.asarray(values,dtype=float)
    return {'count':len(x),'mean_ms':float(x.mean()),'median_ms':float(np.median(x)),
            'p10_ms':float(np.quantile(x,.1)),'p90_ms':float(np.quantile(x,.9)),
            'p95_ms':float(np.quantile(x,.95))}


def read_rows(path):
    with Path(path).open(encoding='utf-8-sig',newline='') as source:
        return list(csv.DictReader(source))


def validate_timing_provenance(path,profile,contract):
    run=Path(path).resolve().parent
    load=lambda name:json.loads((run/name).read_text(encoding='utf-8-sig'))
    proof={'run':str(run),'input_sha256':analysis.sha256(path)}
    if profile=='corona':
        c=load('contract.json');result=load('result.json');route=load('route-validation.json')
        if Path(c['output']).resolve()!=run:
            raise ValueError(f'{run}: helper contract belongs to another process output directory')
        if not(result['status']=='complete' and c['phase']=='timing' and c['kind']=='port'
               and c['capture_raw']is False and c['render_frames']==360 and c['warmup_frames']==120
               and c['benchmark_environment']['VISION_RESTIR_GI_MB_DIAGNOSTIC']=='0' and route['observed']is True):
            raise ValueError(f'{run}: completed no-capture Corona timing route not established')
        if any(c['settings'].get(k)!=v for k,v in [('min_depth',5),('max_depth',16),('rr_threshold',1)]):
            raise ValueError(f'{run}: actual Corona timing transport differs from5/16/1')
        before,after=load('identity-before.json'),load('identity-after.json')
        proof['replay_fix']=analysis.validate_corona_replay_identity(before)
        for group in ['source','runtime']:
            if {p:v['sha256']for p,v in before[group].items()}!={p:v['sha256']for p,v in after[group].items()}:
                raise ValueError(f'{run}: Corona{group} changed during timing')
        identity={g:{p:v['sha256']for p,v in before[g].items()}for g in ['source','runtime']}
        proof['source_runtime_identity_sha256']=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
        frozen=c['frozen_scene_contract'];proof['no_capture_actual']=True
        proof['process_identity']=['Corona_capture_wall_begin_ns',result['capture_wall_window_ns']['begin']]
        evidence=['contract.json','result.json','route-validation.json','identity-before.json','identity-after.json']
    else:
        launch=load('launch-manifest.json');spec=launch['spec']
        if Path(spec['output']).resolve()!=run:
            raise ValueError(f'{run}: launch provenance belongs to another process output directory')
        if launch.get('exit_code')!=0:
            raise ValueError(f'{run}: timing process did not complete successfully')
        expected_path={'rtxpt':'gpu.csv','rtxdi':'capture.gpu.csv','original':'gpu-kernels.csv'}[profile]
        if Path(path).name!=expected_path:
            raise ValueError(f'{run}: timer file is not the expected runtime output')
        frozen=launch['frozen_scene_contract']
        build_path={'rtxpt':run.parents[1]/'rtxpt-evidence/postbuild-verification.json',
                    'rtxdi':run.parents[1]/'rtxdi-build-record.json',
                    'original':run.parents[1]/'reference/build-manifest.json'}[profile]
        build=json.loads(build_path.read_text(encoding='utf-8-sig'))
        expected_hash=build['binary_sha256']if profile=='rtxpt'else build['executable_sha256']
        if launch['executable_sha256']!=expected_hash:
            raise ValueError(f'{run}: executed binary differs from frozen build')
        evidence=['launch-manifest.json'];proof['executable_sha256']=expected_hash
        proof['process_identity']=[launch.get('pid'),launch['start_utc']]
        if profile=='rtxpt':
            actual=load('actual-settings.json');done=load('completed.json')
            if not(actual['raw_write']is False and actual['GI_mode']=='temporalspatial'
                   and done['frames']==360 and done['GI_executed']is True
                   and done['readback_during_timed_phase']is False and done['uncapped']is True
                   and spec['env']['RESTIR_GI_CAPTURE']=='none'
                   and int(spec['experiment']['warmup'])==120 and int(spec['experiment']['frames'])==240):
                raise ValueError(f'{run}: actual RTXPT timing/capture-off contract failed')
            if not(actual['minimum_depth']==5 and actual['max_surface_depth']==16 and actual['first_RR_surface']==6
                   and 'physical_camera_beta_before_current_BSDF'in actual['RR'] and 'no_floor'in actual['RR']):
                raise ValueError(f'{run}: actual RTXPT timing transport differs from5/16/1')
            evidence+=['actual-settings.json','completed.json']
        elif profile=='rtxdi':
            actual=load('capture.settings.json');done=load('capture.result.json')
            if not(actual['mode']=='timing' and actual['raw_capture_flags']==0 and actual['warmup']==120
                   and actual['measured']==240 and done['status']=='complete' and done['rendered_frames']==360):
                raise ValueError(f'{run}: actual RTXDI timing/capture-off contract failed')
            gi=actual['GI']
            if not(gi['min_depth_zero_based']==5 and gi['max_surface_reflections_including_primary']==16
                   and gi['first_RR_after_surface_reflection']==6 and gi['RR_threshold']==1):
                raise ValueError(f'{run}: actual RTXDI timing transport differs from5/16/1')
            shader=build['expected_secondary_shader']
            expected=f"BENCH_SHADER_SELECTED_LIBRARY size={shader['bytes']} fnv64={shader['fnv64']}"
            if expected not in (run/'stdout.txt').read_text(encoding='utf-8',errors='replace'):
                raise ValueError(f'{run}: actual RTXDI shader differs from frozen shader')
            evidence+=['capture.settings.json','capture.result.json']
        else:
            actual=load('benchmark-config.json');done=load('capture-result.json');argv=spec['args']
            if not('--export-raw'not in argv and '--raw-frames'not in argv and actual['reference_mode']is False
                   and done['status']=='complete' and done['frames']==360 and done['warmup']==120):
                raise ValueError(f'{run}: actual original timing/capture-off contract failed')
            if not(actual['min_depth']==5 and actual['max_depth']==16 and actual['RR_first_after_surface']==6 and actual['RR_threshold']==1):
                raise ValueError(f'{run}: actual original timing transport differs from5/16/1')
            evidence+=['benchmark-config.json','capture-result.json']
        proof['no_capture_actual']=True
    physics=['resolution','source_sha256','scene_sha256','geometry_world_float32_triangle_sha256','camera','materials','lighting','transport']
    if any(frozen.get(key)!=contract.get(key)for key in physics):
        raise ValueError(f'{run}: frozen timing physical contract differs from scene contract')
    proof['evidence']=[{'path':str(run/name),'sha256':analysis.sha256(run/name)}for name in evidence]
    return proof


def analyze_repeat(path,profile,warmup=120,measured=240,full_path=None):
    spec=PROFILES[profile]
    frames={}
    repeats={}
    if profile=='corona':
        # The helper binds calibrated GPU events to actual offline frame QPC
        # boundaries. Profile batches also contain setup/separate combine submits.
        native=json.loads(Path(path).read_text(encoding='utf-8-sig'))
        bound=native['gpu']['frames']
        if len({f['sample_index']for f in bound})!=len(bound):
            raise ValueError(f'{path}: duplicate offline sample indices')
        rows=[{'frame':f['sample_index'],'name':d['name'],'ms':d['gpu_ms']}
              for f in bound for d in f['dispatches']]
    else:
        rows=read_rows(path)
    for row in rows:
        frame=int(row[spec['frame']]);name=row[spec['name']];ms=float(row[spec['ms']])
        if not math.isfinite(ms) or ms<0:
            raise ValueError(f'{path} frame{frame} {name}: invalid timestamp')
        if profile=='rtxdi' and ms>0 and row.get('valid')!='1':
            raise ValueError(f'{path} frame{frame} {name}: invalid native timestamp flag')
        if profile=='original':
            ticks=(int(row['end_tick'])-int(row['begin_tick']))*1000/int(row['frequency_hz'])
            if abs(ticks-ms)>1e-7:
                raise ValueError(f'{path} frame{frame} {name}: calibrated tick mismatch')
        if profile in ['rtxdi','original'] and frame>=warmup and row.get('phase')!='measure':
            raise ValueError(f'{path} frame{frame}: incorrect measured phase marker')
        stages=frames.setdefault(frame,{})
        key=(frame,name)
        repeats[key]=repeats.get(key,0)+1
        stages[name]=stages.get(name,0.)+ms
    expected=set(range(warmup,warmup+measured))
    if set(frames)!=set(range(warmup+measured)):
        raise ValueError(f'{path}: expected exact frame coverage0..{warmup+measured-1}')
    records=[]
    values={'GI_exclusive':[],'primary_preparation':[],'GI_plus_primary_preparation':[],'full_GPU_frame':[]}
    values.update({'component_'+name:[]for name in STAGE_GROUPS[profile]})
    full={}
    if full_path:
        for row in read_rows(full_path):
            frame=int(row['frame'])
            if frame in full:raise ValueError(f'{full_path}: duplicate whole GPU frame record')
            full[frame]=float(row['gpu_render_ms'])
        if set(full)!=set(range(warmup+measured)):
            raise ValueError(f'{full_path}: incomplete whole GPU frame coverage')
    for frame in sorted(expected):
        stages=frames[frame]
        prep_names=[n for n in spec['prep'] if not(profile=='corona' and frame==0 and n=='FrameBuffer-SaveSurfaceExtHistory')]
        for name in spec['gi']+prep_names:
            if name not in stages or stages[name]<=0:
                raise ValueError(f'{path} frame{frame}: missing/nonpositive required {name}')
            count=2 if profile=='original' and name=='restirSpatialCS' else 1
            if repeats[(frame,name)]!=count:
                raise ValueError(f'{path} frame{frame}: expected{count} dispatches for {name}')
        gi=sum(stages[name] for name in spec['gi'])
        if profile=='corona':
            count=repeats.get((frame,CORONA_VALIDATION),0)
            expected_count=int(frame>0 and frame%3==0)
            if count!=expected_count:
                raise ValueError(f'{path} frame{frame}: expected{expected_count} native suffix validation events')
            gi+=stages.get(CORONA_VALIDATION,0.)
        prep=sum(stages[name] for name in prep_names)
        components={name:sum(stages.get(stage,0.)for stage in names)for name,names in STAGE_GROUPS[profile].items()}
        if not math.isclose(sum(components.values()),gi,rel_tol=1e-10,abs_tol=1e-9):
            raise ValueError(f'{path} frame{frame}: component sums do not equal strict GI')
        for name,value in components.items():values['component_'+name].append(value)
        if spec['full'] and repeats.get((frame,spec['full']),0)!=1:
            raise ValueError(f'{path} frame{frame}: expected one required whole GPU frame scope')
        if profile=='rtxpt' and 'GI.Exclusive' in stages and not math.isclose(gi,stages['GI.Exclusive'],rel_tol=1e-8,abs_tol=1e-8):
            raise ValueError(f'{path} frame{frame}: GI.Exclusive differs from dispatch sum')
        record={'frame':frame,'GI_exclusive_ms':gi,'primary_preparation_ms':prep,'GI_plus_primary_preparation_ms':gi+prep}
        record['component_ms']=components
        if '_source_batch' in stages:record['source_profile_batch']=stages['_source_batch']
        for key,val in [('GI_exclusive',gi),('primary_preparation',prep),('GI_plus_primary_preparation',gi+prep)]:
            values[key].append(val)
        total=full.get(frame) if full_path else stages.get(spec['full']) if spec['full'] else None
        if total is not None:
            if not math.isfinite(total) or total<=0 or total+1e-6<gi+prep:
                raise ValueError(f'{path} frame{frame}: invalid whole GPU range')
            values['full_GPU_frame'].append(total);record['full_GPU_frame_ms']=total
        records.append(record)
    output={'csv':str(Path(path).resolve()),'profile':profile,'warmup':warmup,'measured':measured,
            'required_GI_scopes':spec['gi'],'primary_preparation_scopes':spec['prep'],
            'scopes':{name:describe(x) if x else None for name,x in values.items()},'frames':records,
            'repeated_rows_summed':[{'frame':f,'stage':n,'rows':count} for (f,n),count in repeats.items() if f in expected and count>1]}
    output['component_scope_groups']=STAGE_GROUPS[profile]
    output['component_note']='Each measured frame enters each component once, including zero when a periodic validation event is absent; component means sum to mean strict GI.'
    if full_path:
        output['full_gpu_source']={'path':str(Path(full_path).resolve()),'sha256':analysis.sha256(full_path)}
    output['measured_stage_event_counts']={name:sum(count for (frame,stage),count in repeats.items()if frame in expected and stage==name)
        for name in spec['gi']+([CORONA_VALIDATION]if profile=='corona'else[])}
    if profile=='corona':
        output['scope_note']='Authoritative result.gpu.frames/QPC event mapping; native suffix validation included on80of240 measured frames. Full GPU range unavailable.'
    if len(values['full_GPU_frame']) not in [0,measured]:
        raise ValueError(f'{path}: incomplete full GPU frame coverage')
    if profile!='corona' and len(values['full_GPU_frame'])!=measured:
        raise ValueError(f'{path}: native whole GPU frame scope is required')
    if not values['full_GPU_frame']:
        output['full_GPU_unavailable_reason']='No measured whole-frame GPU timestamp supplied; CPU wall time is not a substitute.'
    return output,values


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();config=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    base=args.manifest.resolve().parent;resolve=lambda n:(base/n).resolve()
    contract=json.loads(resolve(config['scene_contract']).read_text(encoding='utf-8-sig'))
    if set(config['timing'])!=analysis.QUALITY_RENDERERS:
        raise ValueError('final timing requires all four renderer groups')
    output={'schema':'four-way-multibounce-timing.v1','status':'timing_complete',
        'manifest_path':str(args.manifest.resolve()),'manifest_sha256':analysis.sha256(args.manifest),
        'analysis_script':{'path':str(Path(__file__).resolve()),'sha256':analysis.sha256(__file__)},
        'renderers':{},'notes':[
        'GI sums contain nonoverlapping GI dispatches, excluding CPU, barriers, readbacks and presentation.',
        'Native original validation rays are included in GI because they are required by its algorithm.',
        'Full GPU ranges include each app native unrelated work; report separately from GI and preparation.',
        'Three process-run ranges are descriptive, not independent-frame confidence intervals.'
    ]}
    for label,spec in config['timing'].items():
        if spec['profile']!={'original':'original','rtxpt':'rtxpt','rtxdi':'rtxdi','corona_port':'corona'}[label]:
            raise ValueError(f'{label}: timing profile/renderer mismatch')
        results=[];pooled={}
        if len(spec['repeats'])!=3:raise ValueError(f'{label}: need three process repeats')
        paths=[resolve(r['csv'])for r in spec['repeats']]
        if len(set(paths))!=3 or len({p.parent for p in paths})!=3:
            raise ValueError(f'{label}: need three distinct completed process output directories')
        for repeat in spec['repeats']:
            if repeat.get('warmup')!=120 or repeat.get('measured')!=240:
                raise ValueError(f'{label}: formal timing requires120warmup/240measured frames')
            if label=='original':
                if not repeat.get('full_gpu_csv') or resolve(repeat['full_gpu_csv'])!=resolve(repeat['csv']).parent/'gpu-frames.csv':
                    raise ValueError(f'{label}: require canonical same-run gpu-frames.csv')
            elif repeat.get('full_gpu_csv'):
                raise ValueError(f'{label}: native full GPU scope must come from its own timing artifact')
            proof=validate_timing_provenance(resolve(repeat['csv']),spec['profile'],contract)
            result,values=analyze_repeat(resolve(repeat['csv']),spec['profile'],
                int(repeat.get('warmup',120)),int(repeat.get('measured',240)),
                resolve(repeat['full_gpu_csv']) if repeat.get('full_gpu_csv') else None)
            result['label']=repeat['label'];result['runtime_proof']=proof;results.append(result)
            for name,x in values.items():pooled.setdefault(name,[]).extend(x)
        output['renderers'][label]={'repeats':results,'pooled':{k:describe(x) if x else None for k,x in pooled.items()},
            'repeat_median_ranges_ms':{name:{'min':min(r['scopes'][name]['median_ms'] for r in results),
                                           'max':max(r['scopes'][name]['median_ms'] for r in results)}
                for name in pooled if all(r['scopes'][name] is not None for r in results)}}
        if len({json.dumps(r['runtime_proof']['process_identity'])for r in results})!=3:
            raise ValueError(f'{label}: repeated process provenance cannot establish three independent runs')
        if label=='corona_port'and len({r['runtime_proof']['source_runtime_identity_sha256']for r in results})!=1:
            raise ValueError('Selected Corona timing repeats do not share one source/runtime refreeze')
        if label=='corona_port':
            quality_audit=json.loads((base/'quality-run-audit-v1.json').read_text(encoding='utf-8-sig'))
            expected={r['source_runtime_identity_sha256']for r in quality_audit['runs']['corona_port'].values()}
            if expected!={results[0]['runtime_proof']['source_runtime_identity_sha256']}:
                raise ValueError('Corona quality and timing do not share the same source/runtime refreeze')
        output['renderers'][label]['repeat_mean_ranges_ms']={name:{'min':min(r['scopes'][name]['mean_ms']for r in results),
                                                                'max':max(r['scopes'][name]['mean_ms']for r in results)}
                for name in pooled if all(r['scopes'][name]is not None for r in results)}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(output,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'output':str(args.output.resolve()),'renderers':list(output['renderers'])},indent=2))


if __name__=='__main__':main()
