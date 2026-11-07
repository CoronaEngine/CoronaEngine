"""Read-only snapshot of the eight planned formal reference runs.

Only completed runs enter a producer split or cross-producer calculation. A
single completed half may have provisional radiance statistics, never a split
noise estimate or acceptance. Outputs remain inside this script's directory.
"""
from pathlib import Path
import argparse
import copy
import datetime
import json

import numpy as np

import analyze_multibounce as analysis

ROOT=Path(__file__).resolve().parent
PHYSICS=['resolution','source_sha256','scene_sha256','geometry_world_float32_triangle_sha256',
         'camera','materials','lighting','transport']


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def completed_run(label,item,resolve,contract):
    analysis.validate_reference_input(label,item)
    run=resolve(item['run'])
    marker={'original':'capture-result.json','rtxpt':'completed.json',
            'rtxdi':'capture.result.json','corona_pt':'result.json'}[label]
    if not (run/marker).exists():
        return {'status':'pending','run':str(run),'reason':'Completion marker absent'}
    result=load(run/marker)
    runtime_proof={}
    if label=='corona_pt':
        if result.get('status')!='complete' or not (run/'route-validation.json').exists():
            return {'status':'pending','run':str(run),'reason':'PT completion/route validation pending'}
        c=load(run/'contract.json')
        assert Path(c['output']).resolve()==run,'PT reference contract belongs to another run directory'
        route=load(run/'route-validation.json')
        assert route['observed'] is True,'Corona PT actual route absent'
        assert c['phase']=='reference' and c['kind']=='pt' and c['reference_frames']==1024
        assert c['settings']=={'max_depth':16,'min_depth':5,'rr_threshold':1}
        assert int(c['benchmark_environment']['VISION_BENCH_SAMPLE_OFFSET'])==item['sample_offset']
        assert c['benchmark_environment']['VISION_RESTIR_GI_MULTIBOUNCE_BENCH']=='1'
        frozen=c['frozen_scene_contract']
        means=load(run/'reference-manifest.json')
        assert means['frames']==1024
        before,after=load(run/'identity-before.json'),load(run/'identity-after.json')
        runtime_proof['identity_stable_before_after']={
            group:{p:v['sha256']for p,v in before[group].items()}=={p:v['sha256']for p,v in after[group].items()}
            for group in ['source','runtime']}
        assert all(runtime_proof['identity_stable_before_after'].values()),'Corona source/runtime changed during reference'
        evidence=[marker,'route-validation.json','contract.json','reference-manifest.json','identity-before.json','identity-after.json']
    else:
        if not (run/'launch-manifest.json').exists():
            return {'status':'pending','run':str(run),'reason':'Final process provenance pending'}
        launch=load(run/'launch-manifest.json')
        if launch.get('exit_code') is None:
            return {'status':'pending','run':str(run),'reason':'Renderer completion marker written; process provenance still finalizing'}
        assert launch.get('exit_code')==0, 'Reference process failed'
        assert Path(launch['spec']['output']).resolve()==run,'Reference launch provenance belongs to another run directory'
        assert launch['spec']['experiment']['sample_offset']==item['sample_offset']
        frozen=launch['frozen_scene_contract']
        evidence=[marker,'launch-manifest.json']
        build_path={'rtxpt':ROOT.parent/'rtxpt-evidence/postbuild-verification.json',
                    'rtxdi':ROOT.parent/'rtxdi-build-record.json',
                    'original':ROOT.parent/'reference/build-manifest.json'}[label]
        build=load(build_path)
        expected_binary=build['binary_sha256']if label=='rtxpt'else build['executable_sha256']
        assert launch['executable_sha256']==expected_binary,'Executed binary differs from frozen build'
        runtime_proof={'executable_sha256':launch['executable_sha256'],'matches_build_record':True,
                       'build_record':str(build_path),'build_record_sha256':analysis.sha256(build_path)}
        if label=='rtxpt':
            c=load(run/'actual-settings.json');effective=load(run/'effective-settings.json')
            assert result['frames']==1024 and result['uncapped'] is True
            assert c['GI_mode']=='reference' and c['sample_offset']==item['sample_offset']
            assert c['minimum_depth']==5 and c['max_surface_depth']==16 and c['first_RR_surface']==6
            assert effective['mode']=='reference' and effective['jitter'] is False and effective['NRD'] is False
            evidence+=['actual-settings.json','effective-settings.json']
        elif label=='rtxdi':
            c=load(run/'capture.settings.json')
            assert result['status']=='complete' and result['rendered_frames']==1024
            assert c['mode']=='reference' and c['seed_frame_offset']==item['sample_offset']
            assert c['measured']==1024 and c['warmup']==0 and c['pixel_jitter'] is False
            gi=c['GI']
            assert gi['min_depth_zero_based']==5 and gi['max_surface_reflections_including_primary']==16
            assert gi['first_RR_after_surface_reflection']==6 and gi['RR_threshold']==1
            expected=build['expected_secondary_shader']
            selected=f"BENCH_SHADER_SELECTED_LIBRARY size={expected['bytes']} fnv64={expected['fnv64']}"
            assert selected in (run/'stdout.txt').read_text(encoding='utf-8',errors='replace'),'Actual selected RTXDI secondary shader differs from freeze'
            runtime_proof['selected_secondary_shader_matches_build_record']=True
            evidence+=['capture.settings.json']
        else:
            c=load(run/'benchmark-config.json')
            assert result['status']=='complete' and result['frames']==1024 and result['warmup']==0
            assert result['gi_resolution']==[1920,1080] and result['interval_ms']==0
            assert c['reference_mode'] is True and c['sample_offset']==item['sample_offset']
            assert c['min_depth']==5 and c['max_depth']==16 and c['RR_first_after_surface']==6
            assert c['RR_threshold']==1 and c['fresh_pt_sum_is_sum'] is True
            evidence+=['benchmark-config.json']
    differences=[key for key in PHYSICS if frozen.get(key)!=contract.get(key)]
    assert not differences,'Frozen physical contract mismatch: '+','.join(differences)
    path=resolve(item['path'])
    expected_name={'original':'raw/frame_001023/fresh_pt_sum.bin','rtxpt':'gi-mean.rgba32f',
                   'rtxdi':'capture.mean.rgba32f','corona_pt':'reference.pfm'}[label]
    assert path==(run/expected_name).resolve(),'Reference raw path is not bound to its completed producer run'
    assert path.exists(),'Complete reference missing expected raw mean/sum'
    rgb=analysis.normalized_rgb(analysis.read_reference(path,*contract['resolution']),float(item['divisor']))
    return {'status':'complete_validated','run':str(run),'path':str(path),'sha256':analysis.sha256(path),
            'normalization_divisor':item['divisor'],'storage':item['storage'],'spp':item['spp'],
            'sample_offset':item['sample_offset'],'mean_RGB':rgb.mean(axis=(0,1)).tolist(),
            'full_frame_mean_luminance':float((rgb@analysis.LUMA).mean()),
            'minimum_RGB':float(rgb.min()),'maximum_RGB':float(rgb.max()),
            'runtime_proof':runtime_proof,
            'evidence':[{ 'path':str(run/p),'sha256':analysis.sha256(run/p)}for p in evidence]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path,default=ROOT/'manifest-formal-v1.json')
    args=parser.parse_args()
    manifest=load(args.manifest);base=args.manifest.resolve().parent
    resolve=lambda name:(base/name).resolve()
    contract=load(resolve(manifest['scene_contract']))
    output={'schema':'multibounce-formal-reference-progress.v1',
            'snapshot_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'status':'pending','halves':{},'completed_producer_splits':[],
            'notes':['Single completed halves are provisional; full means and split-noise estimates require both independent1024-spp halves.',
                     'All four producers and all eight completed valid halves are required for the final common-reference review.',
                     'Diagnostic screens are heuristic; no iid pixel/frame confidence test or premature physical pass is asserted.']}
    complete={}
    for label,spec in manifest['references'].items():
        entries={}
        for half in ['a','b']:
            try:
                entries[half]=completed_run(label,spec['half_'+half],resolve,contract)
            except (AssertionError,KeyError,ValueError,FileNotFoundError) as error:
                entries[half]={'status':'audit_issue','error':str(error),'run':str(resolve(spec['half_'+half]['run']))}
        output['halves'][label]=entries
        if all(item['status']=='complete_validated' for item in entries.values()):
            complete[label]=spec
    output['completed_producer_splits']=list(complete)
    if complete:
        guides,mask=analysis.compare_guides(manifest['guides'],resolve,*manifest['resolution'])
        width,height=manifest['resolution']
        regions={'full_frame':np.ones((height,width),bool),'common_valid_primary':mask,
                 'guide_consistent_primary_diagnostic':guides['_agreement_mask']}
        diagnostics,_,_=analysis.analyze_references(complete,resolve,width,height,regions)
        screens={}
        for pair,entry in diagnostics['pairwise'].items():
            screens[pair]={}
            for name in ['full_frame','common_valid_primary']:
                r=entry['regions'][name]
                screens[pair][name]={
                    'relative_mean_difference_abs_below_1pct':abs(r['relative_signed_mean_difference'])<=.01,
                    'cross_MSE_ratio_below_1_25':r['observed_to_expected_cross_MSE']<=1.25,
                    'half_difference_correlation_abs_below_0_1':abs(r['half_difference_centered_correlation'])<=.1}
        output['diagnostic_screens']=screens
        output['diagnostics_file']='reference-progress-diagnostics-v1.json'
        (ROOT/output['diagnostics_file']).write_text(json.dumps(analysis.public({'guides':guides,'references':diagnostics}),indent=2,allow_nan=False)+'\n')
        partial=copy.deepcopy(manifest);partial['references']=complete
        partial['scaffold_note']='Only fully completed/audited producer splits. This remains a progressive diagnostic manifest; false evidence flags suppress quality comparisons.'
        (ROOT/'manifest-references-completed-v1.json').write_text(json.dumps(partial,indent=2)+'\n')
    statuses=[item['status']for halves in output['halves'].values()for item in halves.values()]
    output['completed_halves']=statuses.count('complete_validated')
    output['expected_halves']=len(statuses)
    if 'audit_issue' in statuses:output['status']='audit_issue_requires_review'
    elif all(x=='complete_validated' for x in statuses):output['status']='all_producers_complete_pending_review'
    elif complete:output['status']='partial_producer_splits_only'
    (ROOT/'reference-progress-v1.json').write_text(json.dumps(output,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'status':output['status'],'completed_halves':output['completed_halves'],
                      'completed_producer_splits':list(complete),
                      'halves':{label:{half:item['status']for half,item in halves.items()}for label,halves in output['halves'].items()}},indent=2))


if __name__=='__main__':main()
