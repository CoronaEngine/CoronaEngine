"""CPU-only completion, actual-route, freeze and guide audit of12 quality runs."""
from pathlib import Path
import hashlib
import json

import analyze_multibounce as analysis

ROOT=Path(__file__).resolve().parent
PHYSICS=['resolution','source_sha256','scene_sha256','geometry_world_float32_triangle_sha256',
         'camera','materials','lighting','transport']


def require(value,message):
    if not value:raise ValueError(message)


def main():
    manifest=json.loads((ROOT/'manifest-formal-v1.json').read_text())
    contract=json.loads((ROOT/manifest['scene_contract']).read_text())
    resolve=lambda n:(ROOT/n).resolve()
    output={'schema':'four-way-multibounce-quality-run-audit.v1','status':'pending','runs':{},
            'scope':'Completed128frame quality processes; late32raw frames96..127; all offsets and actual routes/frozen runtimes checked.'}
    identities=set()
    corona_freeze=None
    for label,spec in manifest['quality'].items():
        output['runs'][label]={}
        guide=manifest['guides'][label]
        expected_guides={key:analysis.sha256(resolve(guide[key]))for key in ['position','normal']}
        for repeat in spec['repeats']:
            paths=[resolve(repeat['frame_pattern'].format(frame=f))for f in repeat['frame_indices']]
            run=next(p for p in paths[0].parents if p.parent.name=='runs')
            load=lambda name:json.loads((run/name).read_text(encoding='utf-8-sig'))
            proof={'run':str(run),'sample_offset':repeat['sample_offset']}
            if label=='corona_port':
                c=load('contract.json');done=load('result.json');route=load('route-validation.json')
                require(Path(c['output']).resolve()==run,'Corona quality contract belongs to another output')
                require(done['status']=='complete'and c['phase']=='quality'and c['kind']=='port'
                        and c['capture_raw']is True and c['render_frames']==128 and c['warmup_frames']==0
                        and route['observed']is True,'Actual completed Corona port quality route not established')
                require(int(c['benchmark_environment']['VISION_BENCH_SAMPLE_OFFSET'])==repeat['sample_offset'],'Corona offset mismatch')
                require(all(c['settings'][k]==v for k,v in [('min_depth',5),('max_depth',16),('rr_threshold',1)]),'Corona actual transport mismatch')
                before,after=load('identity-before.json'),load('identity-after.json')
                proof['replay_fix']=analysis.validate_corona_replay_identity(before)
                require(all({p:v['sha256']for p,v in before[g].items()}=={p:v['sha256']for p,v in after[g].items()}
                            for g in ['source','runtime']),'Corona source/runtime changed during quality')
                freeze={g:{p:v['sha256']for p,v in before[g].items()}for g in ['source','runtime']}
                if corona_freeze is None:corona_freeze=freeze
                require(freeze==corona_freeze,'Selected Corona quality repeats do not share one source/runtime refreeze')
                frozen=c['frozen_scene_contract'];process=('Corona',done['capture_wall_window_ns']['begin'])
                proof['source_runtime_stable']=True;proof['source_runtime_consistent_across_repeats']=True
                proof['source_runtime_identity_sha256']=hashlib.sha256(json.dumps(freeze,sort_keys=True).encode()).hexdigest()
                actual=done['effective_after']['settings']
                guides={'position':run/'raw/frame_0000/reference_position_depth.f32','normal':run/'raw/frame_0000/reference_world_normal.f32'}
                evidence=['contract.json','result.json','route-validation.json','identity-before.json','identity-after.json']
            else:
                launch=load('launch-manifest.json');c=launch['spec']['experiment']
                require(launch['exit_code']==0 and Path(launch['spec']['output']).resolve()==run,'Actual successful quality launch absent')
                require(c['sample_offset']==repeat['sample_offset'],'Quality sample offset mismatch')
                frozen=launch['frozen_scene_contract'];process=(launch.get('pid'),launch['start_utc'])
                build_path={'original':ROOT.parent/'reference/build-manifest.json',
                            'rtxpt':ROOT.parent/'rtxpt-evidence/postbuild-verification.json',
                            'rtxdi':ROOT.parent/'rtxdi-build-record.json'}[label]
                build=json.loads(build_path.read_text())
                expected=build['binary_sha256']if label=='rtxpt'else build['executable_sha256']
                require(launch['executable_sha256']==expected,'Executed quality binary differs from frozen build')
                proof['executed_binary_sha256']=expected;evidence=['launch-manifest.json']
                if label=='rtxpt':
                    actual=load('actual-settings.json');done=load('completed.json');effective=load('effective-settings.json')
                    require(done['frames']==128 and done['GI_executed']is True and actual['GI_mode']=='temporalspatial'
                            and actual['raw_write']is True and actual['sample_offset']==repeat['sample_offset'],'Actual RTXPT quality mode/count mismatch')
                    require(actual['minimum_depth']==5 and actual['max_surface_depth']==16 and actual['first_RR_surface']==6
                            and 'physical_camera_beta_before_current_BSDF'in actual['RR']and 'no_floor'in actual['RR'],'Actual RTXPT transport mismatch')
                    require(effective['jitter']is False and effective['NRD']is False and effective['AA']is False,'RTXPT filtering/jitter unexpectedly active')
                    actual=effective;guides={'position':run/'primary-position.rgba32f','normal':run/'primary-geonormal.rgba32f'}
                    evidence+=['actual-settings.json','effective-settings.json','completed.json']
                elif label=='rtxdi':
                    actual=load('capture.settings.json');done=load('capture.result.json');gi=actual['GI']
                    require(done['status']=='complete'and done['rendered_frames']==128 and actual['mode']=='quality'
                            and actual['seed_frame_offset']==repeat['sample_offset']and actual['warmup']==0,'Actual RTXDI quality mode/count mismatch')
                    require(gi['min_depth_zero_based']==5 and gi['max_surface_reflections_including_primary']==16
                            and gi['first_RR_after_surface_reflection']==6 and gi['RR_threshold']==1,'Actual RTXDI transport mismatch')
                    shader=build['expected_secondary_shader'];selected=f"BENCH_SHADER_SELECTED_LIBRARY size={shader['bytes']} fnv64={shader['fnv64']}"
                    require(selected in(run/'stdout.txt').read_text(encoding='utf-8',errors='replace'),'Actual RTXDI secondary shader differs from freeze')
                    require(actual['pixel_jitter']is False and actual['denoiser_mode_enum']==0 and actual['aa_mode_enum']==0,'RTXDI filtering/jitter unexpectedly active')
                    actual=gi;guides={'position':run/'capture.position.rgba32f','normal':run/'capture.normal.rgba32f'}
                    evidence+=['capture.settings.json','capture.result.json']
                else:
                    actual=load('benchmark-config.json');done=load('capture-result.json')
                    require(done['status']=='complete'and done['frames']==128 and done['warmup']==0
                            and done['gi_resolution']==[1920,1080]and actual['reference_mode']is False
                            and actual['sample_offset']==repeat['sample_offset'],'Actual original quality mode/count mismatch')
                    require(actual['min_depth']==5 and actual['max_depth']==16 and actual['RR_first_after_surface']==6
                            and actual['RR_threshold']==1,'Actual original transport mismatch')
                    guides={'position':run/'raw/frame_000000/primary_world_position.bin','normal':run/'raw/frame_000000/primary_world_normal.bin'}
                    evidence+=['benchmark-config.json','capture-result.json']
            require(process not in identities,'A process provenance identity was reused')
            identities.add(process)
            require(all(frozen.get(k)==contract.get(k)for k in PHYSICS),'Frozen quality physical contract mismatch')
            require(repeat['frame_indices']==list(range(96,128))and len(paths)==32,'Late quality frame window mismatch')
            require(all(p.exists()and p.stat().st_size==1920*1080*16 for p in paths),'Missing/malformed late raw capture')
            actual_guides={k:analysis.sha256(p)for k,p in guides.items()}
            require(actual_guides==expected_guides,'Formal primary guides differ from accepted pilot; review actual guide data before comparison')
            proof.update(status='pass',actual_native_settings=actual,primary_guide_sha256=actual_guides,
                         guide_bytes_identical_to_accepted_pilot=True,
                         evidence=[{'path':str(run/name),'sha256':analysis.sha256(run/name)}for name in evidence])
            output['runs'][label][repeat['label']]=proof
    output['status']='pass_all_12_selected_quality_processes'
    (ROOT/'quality-run-audit-v1.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps({'status':output['status'],'process_count':len(identities)},indent=2))


if __name__=='__main__':main()
