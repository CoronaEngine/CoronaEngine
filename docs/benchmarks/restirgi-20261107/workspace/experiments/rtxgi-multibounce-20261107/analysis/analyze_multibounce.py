"""CPU-only multibounce guide/reference/quality analysis; see README.md.

All inputs are scene-linear top-left little-endian float32 RGBA. Per-input
normalization is explicit. There is deliberately no one-bounce radiance bound.
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path

import numpy as np

LUMA = np.array([.2126, .7152, .0722], dtype=np.float64)
REFERENCE_PRODUCERS = {'original', 'rtxpt', 'rtxdi', 'corona_pt'}
QUALITY_RENDERERS = {'original', 'rtxpt', 'rtxdi', 'corona_port'}
CORONA_REPLAY_FIXED_SOURCE_SHA256 = '402d272b0e63b09dcf596d8058ae39abf02cb8728ab84eef259103de1daa2085'
CORONA_SEEDSTREAM_SOURCE_HASHES={CORONA_REPLAY_FIXED_SOURCE_SHA256,
    'd61b3592dad41ae9404378c608168562c562aff4f120288dd6c5ea5004d7d17f',
    '7fc0e614ddf3561f11453fe413daf8c26e5ebd3729d8d264f60129fbbfbf7d42'}


def validate_corona_replay_identity(identity):
    evidence=Path(__file__).resolve().parent/'corona-seed-stream-regression-v1.json'
    record=json.loads(evidence.read_text(encoding='utf-8-sig'))
    actual=identity['source'].get(record['source'],{}).get('sha256')
    if actual!=CORONA_REPLAY_FIXED_SOURCE_SHA256 or record['source_sha256']!=actual:
        raise ValueError('Corona run does not contain the frozen joint-seed-stream-corrected source')
    if set(record['source_hashes'].values())!=CORONA_SEEDSTREAM_SOURCE_HASHES:
        raise ValueError('Corona seed-stream evidence does not match all three frozen source hashes')
    if any(identity['source'].get(path,{}).get('sha256')!=digest for path,digest in record['source_hashes'].items()):
        raise ValueError('Corona literal/integrator source identity differs from the corrected refreeze')
    green=record['green']['report']
    if not(record['status']=='pass'and record['green']['exit_code']==0 and green['sampler_cases']==120
           and green['failed_checks']==[] and len(green['checks'])==19 and all(v is True for v in green['checks'].values())
           and green['unshifted_temporal_negative_control_detected']is True
           and record['protocol']['camera_candidate_replay_temporal_spatial_resolve_keys_shift_together']is True
           and record['protocol']['logical_history_parity_validation_and_temporal_permutation_unchanged']is True):
        raise ValueError('Corona joint seed-stream CPU regression evidence does not pass')
    return {'source':record['source'],'sha256':actual,'regression_evidence':str(evidence),
            'source_hashes':record['source_hashes'],'regression_evidence_sha256':sha256(evidence),
            'cases':120,'guards':19,'mismatches':0}


def validate_reference_input(label, item):
    if label not in REFERENCE_PRODUCERS:
        raise ValueError(f'{label}: unknown formal reference producer')
    storage, divisor = ('SUM', 1024) if label == 'original' else ('MEAN', 1)
    if item.get('spp') != 1024 or item.get('storage') != storage or item.get('divisor') != divisor:
        raise ValueError(f'{label}: requires {storage},1024spp,divisor{divisor}')


def validate_final_manifest(manifest):
    for field, expected in [('guides', QUALITY_RENDERERS), ('quality', QUALITY_RENDERERS),
                            ('references', REFERENCE_PRODUCERS)]:
        if set(manifest.get(field, {})) != expected:
            raise ValueError(f'final {field} must contain exactly {sorted(expected)}')
    for label, spec in manifest['references'].items():
        if spec.get('include_in_common', True) is not True:
            raise ValueError(f'{label}: all four reviewed producers must enter the final common reference')
        for half in ['a', 'b']:
            validate_reference_input(label, spec['half_'+half])
        if [spec['half_'+h]['sample_offset'] for h in ['a','b']] != [100000,200000]:
            raise ValueError(f'{label}: formal reference offsets must be100000/200000')
    for label,spec in manifest['quality'].items():
        if sorted(r['sample_offset'] for r in spec['repeats']) != [0,10000,20000]:
            raise ValueError(f'{label}: formal quality offsets must be0/10000/20000')


def read_rgba(path, width, height):
    path = Path(path)
    expected = width * height * 16
    if path.stat().st_size != expected:
        raise ValueError(f'{path}: expected {expected} bytes; found {path.stat().st_size}')
    return np.memmap(path, dtype='<f4', mode='r', shape=(height, width, 4))


def read_reference(path, width, height):
    path = Path(path)
    if path.suffix.lower() != '.pfm':
        return read_rgba(path, width, height)
    with path.open('rb') as stream:
        if stream.readline().strip() != b'PF':
            raise ValueError(f'{path}: expected RGB PFM')
        dimensions = [int(v) for v in stream.readline().split()]
        scale = float(stream.readline())
        if dimensions != [width, height] or scale != -1.:
            raise ValueError(f'{path}: expected {width}x{height} little-endian unit-scale PFM')
        raw = np.frombuffer(stream.read(), dtype='<f4')
    if raw.size != width * height * 3:
        raise ValueError(f'{path}: incorrect PFM payload size')
    return raw.reshape(height, width, 3)[::-1]


def normalized_rgb(raw, divisor):
    if not math.isfinite(divisor) or divisor <= 0:
        raise ValueError('explicit reference divisor must be finite and positive')
    rgb = np.asarray(raw[..., :3], dtype=np.float64) / divisor
    if not np.isfinite(rgb).all() or np.any(rgb < -1e-7):
        raise ValueError('nonfinite or negative RGB radiance')
    return rgb


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def ratio(a, b):
    return float(a / b) if b > 0 else None


def reference_region(a, b, mask):
    mean = (a[mask] + b[mask]) * .5
    diff = a[mask] - b[mask]
    return {'pixel_count': int(mask.sum()), 'mean_luminance': float(mean.mean()),
            'reference_L2_rms': float(np.sqrt(np.mean(mean * mean))),
            'split_difference_rmse': float(np.sqrt(np.mean(diff * diff))),
            'estimated_mean_reference_noise_mse': float(np.mean(diff * diff) * .25),
            'estimated_mean_reference_noise_rmse': float(np.sqrt(np.mean(diff * diff) * .25)),
            'signed_half_mean_difference': float(diff.mean())}


def quality_regions(mean, m2, dof, frame_mse, frames, reference, ref_noise, regions):
    output = {}
    variance = m2 / dof
    for name, mask in regions.items():
        r, x = reference[mask], mean[mask]
        power = float(np.mean(r * r))
        reference_mean = float(r.mean())
        noise_mse = float(ref_noise[mask].mean())
        mse = frame_mse[name] / frames
        time_mean_mse = float(np.mean((x - r) ** 2))
        temporal_var = float(variance[mask].mean())
        output[name] = {
            'pixel_count': int(mask.sum()), 'mean_luminance': float(x.mean()),
            'signed_mean_bias': float((x - r).mean()),
            'relative_signed_mean_bias': ratio(float((x - r).mean()), reference_mean),
            'temporal_sample_std_rms': math.sqrt(temporal_var),
            'temporal_RMS_noise_over_reference_L2': math.sqrt(temporal_var / power) if power > 0 else None,
            'mean_per_frame_mse': mse, 'mean_per_frame_rmse': math.sqrt(mse),
            'per_frame_L2_NRMSE_rms': math.sqrt(mse / power) if power > 0 else None,
            'reference_noise_adjusted_frame_L2_NRMSE': math.sqrt(max(0., mse-noise_mse) / power) if power > 0 else None,
            'time_mean_rmse': math.sqrt(time_mean_mse),
            'time_mean_L2_NRMSE': math.sqrt(time_mean_mse / power) if power > 0 else None,
            'reference_noise_adjusted_time_mean_L2_NRMSE': math.sqrt(max(0., time_mean_mse-noise_mse) / power) if power > 0 else None,
        }
    return output


def summarize_sequence(luminances, reference, ref_noise, regions):
    mean = np.zeros_like(reference)
    m2 = np.zeros_like(reference)
    previous = None
    lag_product = np.zeros_like(reference)
    lag_first = np.zeros_like(reference)
    lag_second = np.zeros_like(reference)
    frame_mse = {name: 0. for name in regions}
    frame_records = []
    for count, x in enumerate(luminances, 1):
        delta = x - mean
        mean += delta / count
        m2 += delta * (x - mean)
        if previous is not None:
            lag_product += previous * x
            lag_first += previous
            lag_second += x
        previous = x
        record = {}
        for name, mask in regions.items():
            mse = float(np.mean((x[mask] - reference[mask]) ** 2))
            frame_mse[name] += mse
            record[name] = {'mean_luminance': float(x[mask].mean()), 'mse': mse}
        frame_records.append(record)
    if len(frame_records) < 2:
        raise ValueError('need at least two distinct frames in each sequence')
    n = len(frame_records)
    lag_numerator = lag_product - lag_first * lag_second / (n - 1)
    result = {'frame_count': n, 'temporal_variance_degrees_of_freedom': n-1, 'lag_pairs': n-1,
              'regions': quality_regions(mean, m2, n-1, frame_mse, n, reference, ref_noise, regions),
              'frames': frame_records,
              '_mean': mean, '_m2': m2, '_frame_mse': frame_mse, '_lag_numerator': lag_numerator}
    for name, mask in regions.items():
        result['regions'][name]['lag1_covariance_over_variance'] = (
            ratio(float((lag_numerator[mask] / (n-2)).mean()), float((m2[mask] / (n-1)).mean()))
            if n > 2 else None)
    return result


def pool_sequences(sequences, reference, ref_noise, regions):
    n = sum(s['frame_count'] for s in sequences)
    dof = sum(s['temporal_variance_degrees_of_freedom'] for s in sequences)
    mean = sum(s['_mean'] * s['frame_count'] for s in sequences) / n
    m2 = sum(s['_m2'] for s in sequences)
    frame_mse = {name: sum(s['_frame_mse'][name] for s in sequences) for name in regions}
    result = {'frame_count': n, 'repeat_count': len(sequences),
              'temporal_variance_degrees_of_freedom': dof,
              'lag_pairs': sum(s['lag_pairs'] for s in sequences),
              'regions': quality_regions(mean, m2, dof, frame_mse, n, reference, ref_noise, regions),
              'repeat_ranges': {}}
    lag_dof = sum(max(0, s['frame_count']-2) for s in sequences)
    for name, mask in regions.items():
        lag_cov = sum(s['_lag_numerator'][mask].mean() for s in sequences) / lag_dof if lag_dof else 0.
        result['regions'][name]['lag1_covariance_over_variance'] = ratio(lag_cov, float((m2[mask]/dof).mean()))
        metrics = sequences[0]['regions'][name]
        result['repeat_ranges'][name] = {
            key: {'min': min(s['regions'][name][key] for s in sequences),
                  'max': max(s['regions'][name][key] for s in sequences)}
            for key in metrics if key != 'pixel_count' and all(s['regions'][name][key] is not None for s in sequences)}
    return result


def public(value):
    if isinstance(value, dict):
        return {k: public(v) for k, v in value.items() if not k.startswith('_')}
    if isinstance(value, list):
        return [public(v) for v in value]
    return value


def canonical_xyz(raw, scale):
    return np.asarray(raw[..., :3], dtype=np.float64) * np.asarray(scale, dtype=np.float64)


def compare_guides(guides, resolve, width, height):
    loaded, output = {}, {}
    agreement = np.ones((height, width), bool)
    for label, spec in guides.items():
        p = read_rgba(resolve(spec['position']), width, height)
        n = read_rgba(resolve(spec['normal']), width, height)
        validity = spec['validity']
        v = read_rgba(resolve(validity['path']), width, height)[..., int(validity['channel'])] > float(validity.get('threshold', 0))
        if not np.isfinite(p).all() or not np.isfinite(n).all() or not v.any():
            raise ValueError(f'{label}: invalid/empty primary guide')
        scale = spec.get('internal_to_canonical_scale', [1, 1, 1])
        loaded[label] = canonical_xyz(p, scale), canonical_xyz(n, scale), v
        output[label] = {'valid_pixels': int(v.sum()), 'position_sha256': sha256(resolve(spec['position'])),
                         'normal_sha256': sha256(resolve(spec['normal'])), 'validity': validity,
                         'internal_to_canonical_scale': scale}
    comparisons = {}
    for left, right in itertools.combinations(loaded, 2):
        p, n, v = loaded[left]
        q, m, u = loaded[right]
        common = v & u
        d = np.linalg.norm(p[common]-q[common], axis=1)
        normals_a, normals_b = n[common], m[common]
        denom = np.linalg.norm(normals_a, axis=1) * np.linalg.norm(normals_b, axis=1)
        dot = np.sum(normals_a * normals_b, axis=1) / np.maximum(denom, 1e-30)
        pair_agreement = np.zeros((height, width), bool)
        pair_agreement[common] = (d <= .001) & (dot >= .999)
        agreement &= pair_agreement
        comparisons[left+'__'+right] = {
            'common_valid_pixels': int(common.sum()), 'validity_mismatch_pixels': int((v!=u).sum()),
            'position_distance_m_p50_p95_p99_p999_max': np.percentile(d,[50,95,99,99.9,100]).tolist(),
            'position_pixels_above_1mm': int((d > .001).sum()),
            'normal_dot_min_p001_p1_p50': np.percentile(dot,[0,.01,1,50]).tolist(),
            'normal_pixels_below_dot_0999': int((dot < .999).sum())}
    masks = [x[2] for x in loaded.values()]
    common = np.logical_and.reduce(masks) if masks else np.ones((height,width),bool)
    if not common.any():
        raise ValueError('empty common-valid guide mask')
    return {'inputs': output, 'pairwise': comparisons,
            'guide_consistent_pixels': int((agreement & common).sum()),
            'guide_consistency_diagnostic_definition': 'all pairwise valid positions within1mm and normal dot>=.999; edge exclusion is diagnostic only',
            '_agreement_mask': agreement & common}, common


def analyze_references(specs, resolve, width, height, regions):
    arrays, metadata, halves_seen = {}, {}, set()
    sample_counts = set()
    for label, spec in specs.items():
        data, meta = [], []
        if spec['half_a']['sample_offset'] == spec['half_b']['sample_offset']:
            raise ValueError(f'{label}: reference half offsets must differ')
        for name in ['half_a','half_b']:
            item = spec[name]
            validate_reference_input(label, item)
            sample_counts.add(int(item['spp']))
            path = resolve(item['path'])
            digest = sha256(path)
            if digest in halves_seen:
                raise ValueError(f'{label}: duplicate reference half bytes')
            halves_seen.add(digest)
            rgb = normalized_rgb(read_reference(path,width,height), float(item['divisor']))
            data.append(rgb @ LUMA)
            meta.append(dict(item,sha256=digest,RGB_min=float(rgb.min()),RGB_max=float(rgb.max()),
                             max_RGB_channel_spread=float(np.ptp(rgb,axis=2).max())))
        a,b=data
        arrays[label] = a,b
        metadata[label] = {'halves':meta,'regions':{r:reference_region(a,b,m) for r,m in regions.items()},
                           'include_in_common':bool(spec.get('include_in_common', True)),
                           'evidence':spec.get('evidence',[])}
    if len(sample_counts) != 1 or min(sample_counts) <= 0:
        raise ValueError('all compared reference halves must have equal positive spp')
    pairs = {}
    for left,right in itertools.combinations(arrays,2):
        a,b=arrays[left];c,d=arrays[right]
        delta=(a+b-c-d)*.5
        results={}
        for name,mask in regions.items():
            x,y=(a-b)[mask],(c-d)[mask]
            observed=float(np.mean(delta[mask]**2))
            expected=float(np.mean(x*x+y*y)*.25)
            xc,yc=x-x.mean(),y-y.mean()
            common_scale=float(((a+b+c+d)*.25)[mask].mean())
            results[name]={'signed_mean_difference':float(delta[mask].mean()),
                'relative_signed_mean_difference':ratio(float(delta[mask].mean()),common_scale),
                'cross_mean_rmse':math.sqrt(observed),
                'expected_independent_cross_mean_noise_rmse':math.sqrt(expected),
                'observed_to_expected_cross_MSE':ratio(observed,expected),
                'noise_adjusted_cross_rmse':math.sqrt(max(0.,observed-expected)),
                'half_difference_centered_correlation':ratio(float(np.dot(xc,yc)),float(np.linalg.norm(xc)*np.linalg.norm(yc)))}
        # Coarse tile means reveal structured geometry/transport disagreement.
        tiles=[]
        tile_columns, tile_rows = min(width,16), min(height,9)
        for ys in np.array_split(np.arange(height),tile_rows):
            for xs in np.array_split(np.arange(width),tile_columns):
                tiles.append(float(delta[np.ix_(ys,xs)].mean()))
        pairs[left+'__'+right]={'regions':results,'tile_grid':[tile_columns,tile_rows],
            'tile_signed_mean_differences':tiles,'tile_max_abs_mean_difference':max(map(abs,tiles))}
    selected=[label for label,spec in specs.items() if spec.get('include_in_common',True)]
    if not selected:
        raise ValueError('no references selected for common mean')
    common_a=sum(arrays[label][0] for label in selected)/len(selected)
    common_b=sum(arrays[label][1] for label in selected)/len(selected)
    reference=(common_a+common_b)*.5
    noise=(common_a-common_b)**2*.25
    result={'producers':metadata,'pairwise':pairs,'common':{
        'producers':selected,'weighting':'equal producer means; each producer has two equal-spp halves',
        'regions':{name:reference_region(common_a,common_b,mask) for name,mask in regions.items()}}}
    return result,reference,noise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--diagnostics-only',action='store_true')
    args=parser.parse_args()
    manifest=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    gates=manifest.get('comparison_evidence',{})
    required=['actual_guides_reviewed','fresh_reference_agreement_reviewed','actual_route_and_transport_reviewed','source_and_runtime_freeze_reviewed']
    approved=all(gates.get(key) is True for key in required)
    if not args.diagnostics_only and approved:
        validate_final_manifest(manifest)
    base=args.manifest.resolve().parent
    resolve=lambda path:(base/path).resolve()
    width,height=manifest['resolution']
    contract_path=resolve(manifest['scene_contract'])
    contract=json.loads(contract_path.read_text(encoding='utf-8-sig'))
    if [width, height] != contract['resolution']:
        raise ValueError('manifest resolution differs from frozen scene contract')
    if contract['transport']['min_depth']!=5 or contract['transport']['max_depth']!=16 or contract['transport']['rr_threshold']!=1:
        raise ValueError('requires frozen multibounce5/16/1 scene contract')
    guides,mask=compare_guides(manifest['guides'],resolve,width,height)
    regions={'full_frame':np.ones((height,width),bool),'common_valid_primary':mask}
    if guides['_agreement_mask'].any():
        regions['guide_consistent_primary_diagnostic']=guides['_agreement_mask']
    references,reference,noise=analyze_references(manifest['references'],resolve,width,height,regions)
    output={'schema':'four-way-multibounce-analysis.v1','manifest_path':str(args.manifest.resolve()),
        'manifest_sha256':sha256(args.manifest),
        'analysis_script':{'path':str(Path(__file__).resolve()),'sha256':sha256(__file__)},
        'scene_contract_sha256':sha256(contract_path),'resolution':[width,height],
        'guides':guides,'references':references,'comparison_evidence':manifest.get('comparison_evidence',{}),
        'quality':{},'exclusions':manifest.get('exclusions',[]),
        'notes':[
            'There is no one-bounce fresh radiance upper bound for this multibounce benchmark.',
            'Temporal reuse correlates frames. Repeat ranges and lag diagnostics are descriptive; no iid confidence interval is asserted.',
            'Pooled temporal variance sums within-repeat squared deviations with N-repeat_count degrees of freedom. No lag pair crosses repeat boundaries.',
            'Reference noise adjustment uses one independent half split; it is diagnostic, not proof of zero bias. Correlation or systematic shared bias can defeat a split test.',
            'L2 normalization uses the observed common reference power. Luminance is Rec.709 scene-linear RGB; positive brightness bias means too bright.',
            'Guide/reference agreement and actual route/source/runtime evidence require review; metrics alone do not validate transport.'
        ]}
    output['status']='diagnostics_only' if args.diagnostics_only else 'quality_complete' if approved else 'quality_blocked_pending_evidence'
    if not args.diagnostics_only and approved:
        all_paths=set()
        for label,spec in manifest.get('quality',{}).items():
            repeats=[]
            for repeat in spec['repeats']:
                names=repeat.get('frames') or [repeat['frame_pattern'].format(frame=f) for f in repeat['frame_indices']]
                paths=[resolve(p) for p in names]
                if len(paths)!=32 or len(set(paths))!=32 or any(p in all_paths for p in paths):
                    raise ValueError(f'{label}: each repeat must contain32 unique non-reused frames')
                all_paths.update(paths)
                if repeat['frame_indices']!=list(range(96,128)):
                    raise ValueError(f'{label}: measured late indices must be96..127')
                if repeat['sample_offset'] in [r['sample_offset'] for r in repeats]:
                    raise ValueError(f'{label}: repeat sample offsets must differ')
                luminances=(normalized_rgb(read_rgba(p,width,height),1.)@LUMA for p in paths)
                sequence=summarize_sequence(luminances,reference,noise,regions)
                sequence.update({key:repeat[key] for key in ['label','sample_offset','frame_indices']})
                sequence['input_files']=[{'path':str(p),'bytes':p.stat().st_size,'sha256':sha256(p)} for p in paths]
                repeats.append(sequence)
            if len(repeats)!=3:
                raise ValueError(f'{label}: need three independent process/offset repeats')
            output['quality'][label]={'pooled':pool_sequences(repeats,reference,noise,regions),
                                      'repeats':public(repeats),'native_settings':spec.get('native_settings',{})}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(public(output),indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'output':str(args.output.resolve()),'status':output['status'],
                      'reference_producers':list(manifest['references']),
                      'quality_renderers':list(output['quality'])},indent=2))


if __name__=='__main__':
    main()
