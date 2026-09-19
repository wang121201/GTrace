#!/usr/bin/env python3
"""Derive a sealed host-argument layout plan from a successful native census."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
from consumer import artifact_pins, enriched_calls, integer, parse_json, pin, read, validate_metadata
from support import CAPTURE_SHA, R1_SHA, R2_SHA, identity, load_r1, need, sha as sha_file

SCHEMA = 'SG_NATIVE_ARGUMENT_CAPTURE_PLAN_V1'
KINDS = 'sha256_nvbit_decoded_instruction_rows_v1'
ALLOWED_ATTRIBUTE = {'id': 6, 'name': 'PROGRAMMATIC_STREAM_SERIALIZATION', 'value': 0,
                     'metadata_decoded': True, 'dynamic_scheduling_qualified': False}
# Opaque identities are audit data only. Their first-occurrence binding ordinals
# preserve alias topology in an independent process; stream zero is semantic.
REFERENCE_ONLY = {'native_launch_id', 'function_id', 'call_id', 'context_id', 'stream_u64'}


def compact(value):
    return json.dumps(value, ensure_ascii=True, separators=(',', ':'))


def row_bound(p):
    """Exact serializer envelope with all variable unsigned identities at UINT64_MAX.

    Current plan strings are ASCII. Hex/hash widths are fixed; pointer contents
    do not change output size. This bound includes the terminating newline.
    """
    u = (1 << 64) - 1
    row = dict(schema='SG_NATIVE_ARGUMENT_VECTOR_V1', sequence=u,
        native_launch_binding=dict(process=dict(pid=u, start_ticks=u), native_launch_id=u, source_launch_key=p['source_launch_key']),
        source_launch_key=p['source_launch_key'], epoch_id=p['epoch_id'], epoch_launch_ordinal=p['epoch_launch_ordinal'],
        forward_id=p['forward_id'], phase=p['phase'], cuda_api=p['cuda_api'], layer=p['layer_id'],
        module_scope=p['module_scope'], module_call_id=u, module_kernel_ordinal=p['module_kernel_ordinal'],
        context_id=u, function_id=u, stream_u64=0, code_sha256=p['code_sha256'], code_sha256_kind=p['code_sha256_kind'],
        parameter_layout_sha256=p['parameter_layout_sha256'], grid=p['grid'], block=p['block'],
        dynamic_shared_bytes=p['dynamic_shared_bytes'], static_shared_bytes=p['static_shared_bytes'],
        registers=p['registers'], local_bytes_per_thread=p['local_bytes_per_thread'], launch_attributes=p['launch_attributes'],
        argument_transport='kernelParams', capture_before_original_launch=True, device_memory_dereferenced=False,
        arguments=[dict(index=i, size_bytes=size, parameter_buffer_offset=None, raw_bytes_hex='ff'*size, sha256='f'*64)
                   for i, size in enumerate(p['argument_sizes'])])
    return len(compact(row).encode('ascii')) + 1


def validate_plan(plan):
    need(plan['schema'] == SCHEMA and plan['raw_argument_values_captured'] is False, 'layout-only plan schema')
    phases = plan['phases'];rows = plan['launches'];limits = plan['limits']
    need(type(phases) is list and 1 <= len(phases) <= 64 and len(set(phases)) == len(phases), 'bounded unique plan phases')
    need(type(rows) is list and 0 < len(rows) <= 100000, 'bounded plan launches')
    counts = collections.Counter();apis = collections.Counter();arguments = raw = 0;maximum_args = maximum_arg = maximum_launch = 0
    ids = {name: {} for name in REFERENCE_ONLY - {'native_launch_id'}}
    last_epoch = 1;last_lid = -1;modules = collections.Counter();aliases = 0
    for p in rows:
        e = integer(p['epoch_id'], 1, len(phases));o = integer(p['epoch_launch_ordinal'])
        need(last_epoch <= e <= last_epoch + 1 and o == counts[e], 'strict contiguous epoch/launch plan order');last_epoch = e
        need(p['source_launch_key'] == 'epoch-%d-launch-%d' % (e,o), 'exact source key')
        counts[e] += 1
        need(p['phase'] == phases[e-1] and p['forward_id'] == e-1 and p['role'] == 'measurement', 'plan phase/forward/role')
        integer(p['layer_id'], -1, 31)
        for field, cap in [('phase',128),('module_scope',2048),('function_name',32768)]:
            value = p[field];need(type(value) is str and 0 < len(value) <= cap and value.isascii(), 'bounded ASCII plan metadata')
        need(p['code_sha256_kind'] == KINDS, 'decoded SASS identity kind')
        for field in ['code_sha256','parameter_layout_sha256']:
            value=p[field];need(type(value) is str and len(value)==64 and all(c in '0123456789abcdef' for c in value), 'plan hash text')
        sizes=p['argument_sizes'];need(type(sizes) is list and 1<=len(sizes)<=256, 'argument vector bound')
        for n in sizes:integer(n,1,65536)
        need(sum(sizes)<=1<<20, 'launch payload bound')
        need(hashlib.sha256(compact(sizes).encode()).hexdigest()==p['parameter_layout_sha256'], 'plan ABI layout SHA')
        for field,cap in [('grid',(1<<32)-1),('block',1024)]:
            need(type(p[field]) is list and len(p[field])==3, 'plan geometry length')
            for n in p[field]:integer(n,1,cap)
        for f in ['dynamic_shared_bytes','static_shared_bytes','registers','local_bytes_per_thread']:integer(p[f],0,(1<<32)-1)
        need(p['cuda_api'] in ['cuLaunchKernel','cuLaunchKernelEx','cuLaunchKernel_ptsz','cuLaunchKernelEx_ptsz'], 'supported native API')
        attrs=p['launch_attributes'];need(attrs==[] or attrs==[ALLOWED_ATTRIBUTE], 'unsupported or opaque launch attributes')
        need(p['stream_u64']==0 and p['context_id']>0, 'single default stream/current context plan')
        lid=integer(p['native_launch_id']);need(lid>last_lid, 'reference native launch order');last_lid=lid
        for f,mapping in ids.items():
            v=integer(p[f]);mapping.setdefault(v,len(mapping));need(p[f+'_binding']==mapping[v], 'reference identity binding topology')
        mk=(e,p['call_id']);need(p['module_kernel_ordinal']==modules[mk], 'module ordinal from current call identity');modules[mk]+=1
        aliases += p['dynamic_module'] != p['module_scope']
        arguments += len(sizes);raw += sum(sizes);apis[p['cuda_api']]+=1
        maximum_args=max(maximum_args,len(sizes));maximum_arg=max(maximum_arg,max(sizes));maximum_launch=max(maximum_launch,sum(sizes))
    need(set(counts)==set(range(1,len(phases)+1)), 'complete phase population')
    expected=dict(launches=len(rows),arguments=arguments,raw_bytes=raw,max_arguments=maximum_args,max_argument_bytes=maximum_arg,max_launch_bytes=maximum_launch)
    need(all(type(limits[k]) is int and limits[k]==v for k,v in expected.items()), 'exact generated argument totals')
    need(raw<=64<<20 and len(ids['context_id'])==len(ids['stream_u64'])==1, 'single-context/default-stream plan safety')
    need(plan['phase_counts']=={p:counts[i+1] for i,p in enumerate(phases)} and plan['cuda_api_counts']==dict(apis), 'plan phase/API totals')
    need(plan['shared_module_alias_launches']==aliases, 'plan ancestry alias total')
    bounds=[row_bound(p) for p in rows]
    need(plan['serialized_bounds']=={'maximum_row_bytes_including_newline':max(bounds),'total_bytes_including_newlines':sum(bounds),'method':'exact SG_NATIVE_ARGUMENT_VECTOR_V1 serializer envelope; fresh uint64 IDs use 20 digits; ASCII plan strings; fixed-width hex and SHA'}, 'serialized bound derivation')
    need(type(limits['max_row_bytes']) is int and max(bounds)<=limits['max_row_bytes']<=1<<20, 'sealed row quota')
    need(type(limits['max_file_bytes']) is int and sum(bounds)<=limits['max_file_bytes']<=256<<20, 'sealed file quota')
    need(plan['source_metadata_bytes_before_finish'] + limits['max_file_bytes'] + (128<<10) <= 1<<30, 'seventh journal plus observed metadata/finish fits total cap')
    return plan


def compile_header(raw):
    need(len(raw)<=64<<20, 'bounded header input plan');plan=validate_plan(parse_json(raw));digest=hashlib.sha256(raw).hexdigest()
    templates=[];lookup={};indexes=[]
    # Epoch, phase and the two ordinals live in a compact index table. All
    # other fields are deduplicated exact Entry templates, including scope.
    fields=['layer_id','module_scope','code_sha256','code_sha256_kind','parameter_layout_sha256','launch_attributes','cuda_api','grid','block','dynamic_shared_bytes','static_shared_bytes','registers','local_bytes_per_thread','argument_sizes']
    for p in plan['launches']:
        value={k:p[k] for k in fields};key=compact(value)
        if key not in lookup:lookup[key]=len(templates);templates.append(value)
        indexes.append([lookup[key],p['epoch_id'],p['epoch_launch_ordinal'],p['module_kernel_ordinal']])
    out=['#pragma once','#include "argument_capture.h"','namespace sgargs {',f'static const char* ARGUMENT_PLAN_SHA="{digest}";', 'inline Limits make_limits(){Limits l;']
    for k,v in plan['limits'].items():out.append(f'l.{k}={v}ull;')
    out+=['return l;}','inline std::vector<Entry> make_plan(){','std::vector<Entry> t;t.reserve(%d);'%len(templates)]
    strings={'module':'module_scope','code':'code_sha256','code_kind':'code_sha256_kind','layout':'parameter_layout_sha256','api':'cuda_api'}
    for p in templates:
        out+=['{Entry x;',f'x.layer={p["layer_id"]};']
        for member,f in strings.items():out.append(f'x.{member}={json.dumps(p[f])};')
        out.append('x.attrs='+json.dumps(compact(p['launch_attributes']))+';')
        for f in ['grid','block']:out.append('x.%s={{%s}};'%(f,','.join(map(str,p[f]))))
        for member,f in [('dynamic_shared','dynamic_shared_bytes'),('static_shared','static_shared_bytes'),('registers','registers'),('local_bytes','local_bytes_per_thread')]:out.append(f'x.{member}={p[f]}ull;')
        out+=['x.sizes={'+','.join(map(str,p['argument_sizes']))+'};t.push_back(x);}']
    out+=['static const char* phases[]={'+','.join(json.dumps(p) for p in plan['phases'])+'};', 'static const uint64_t rows[][4]={']
    out += ['{'+','.join(map(str,row))+'},' for row in indexes]
    out+=['};','std::vector<Entry> v;v.reserve(%d);'%len(indexes),'for(const auto& r:rows){Entry x=t.at(r[0]);x.epoch=r[1];x.ordinal=r[2];x.module_ordinal=r[3];x.phase=phases[x.epoch-1];v.push_back(x);}return v;}','}']
    header='\n'.join(out)+'\n';need(len(header.encode())<=64<<20, 'bounded generated header');return header


def make_plan(source):
    source=Path(source);controller=read(source/'controller.json');r1=load_r1()
    need(controller['status']=='PASS_NATIVE_METADATA_CENSUS_ONLY' and controller['execute'] is True, 'successful real observer controller required')
    need(controller['workload_manifest_sha256']==CAPTURE_SHA and controller['validator_manifest_sha256']==R1_SHA and controller['package']['manifest_sha256']==R2_SHA, 'source package identities')
    r2=next((p for p in [Path(__file__).parent.parent/'native-observer-r2',Path(__file__).parent.parent/'observer-r2'] if (p/'upload-manifest.json').exists()),None)
    need(r2 is not None and identity(r2,R2_SHA)==controller['package'], 'frozen r2 package exact identity')
    census=read(source/'native-census.json');need(sha_file(source/'native-census.json')==controller['native_census_sha256'], 'successful census receipt SHA')
    manifest=read(source/'native/artifacts/manifest.json');c=manifest['input_contract'];need(c==r1.workload.contract(), 'exact frozen workload contract')
    build=read(source/'build/build.json');need(sha_file(source/'build/build.json')==controller['build']['receipt_sha256'] and build['status']=='PASS_BUILD_ONLY_NO_GPU', 'source build receipt')
    need(sha_file(source/'build/observer.so')==controller['build']['binary']['sha256'] and (source/'build/observer.so').stat().st_size==controller['build']['binary']['bytes'], 'source built observer SHA')
    artifacts=artifact_pins(source/'native',c)
    derived,root,finish,before,after,modules=validate_metadata(source/'native',c,source)
    need(derived==census, 'independent recomputed census differs from successful original')
    rows,aliases=enriched_calls(census,before,modules)
    sizes=[s for row in rows for s in row['argument_sizes']];bounds=[row_bound(p) for p in rows]
    file_bound=1
    while file_bound<sum(bounds):file_bound*=2
    plan=dict(schema=SCHEMA,raw_argument_values_captured=False,input_contract_sha256=c['sha256'],phases=c['phases'],
        phase_counts=census['phase_counts'],cuda_api_counts=dict(collections.Counter(r['cuda_api'] for r in rows)),
        limits=dict(launches=len(rows),arguments=len(sizes),raw_bytes=sum(sizes),max_arguments=max(len(p['argument_sizes']) for p in rows),max_argument_bytes=max(sizes),max_launch_bytes=max(sum(p['argument_sizes']) for p in rows),max_row_bytes=16384,max_file_bytes=file_bound),
        source_metadata_bytes_before_finish=finish['metadata_bytes_before_finish'],
        serialized_bounds=dict(maximum_row_bytes_including_newline=max(bounds),total_bytes_including_newlines=sum(bounds),method='exact SG_NATIVE_ARGUMENT_VECTOR_V1 serializer envelope; fresh uint64 IDs use 20 digits; ASCII plan strings; fixed-width hex and SHA'),
        topology=dict(contexts=1,streams=1,default_stream_required=True,opaque_ids='rebind by first-observed identity equivalence; preserve aliases and module ancestry'),
        shared_module_alias_launches=aliases,measured_decoded_code_hashes=len({p['code_sha256'] for p in rows}),
        source_evidence=dict(controller=pin(source/'controller.json'),census=pin(source/'native-census.json'),observer_finish=pin(root/'finish.json'),journals=finish['files'],artifacts=artifacts,build=pin(source/'build/build.json'),observer_binary=pin(source/'build/observer.so'),workload_manifest_sha256=CAPTURE_SHA,r1_manifest_sha256=R1_SHA,r2_manifest_sha256=R2_SHA,reference_process=census['process']),
        qualification=dict(host_argument_values=False,dynamic_memory_addresses=False,dynamic_program_execution=False,typed_objects_or_relocation=False,native_model_admitted=False),launches=rows)
    return validate_plan(plan)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,default=Path(__file__).parent)
    a=p.parse_args();plan=make_plan(a.source);raw=(json.dumps(plan,ensure_ascii=True,indent=2)+'\n').encode();header=compile_header(raw)
    a.output.mkdir(parents=True,exist_ok=True);(a.output/'argument-plan.json').write_bytes(raw);(a.output/'argument_plan.h').write_text(header)
    print(json.dumps({'status':'PASS_LAYOUT_PLAN_ONLY','plan':pin(a.output/'argument-plan.json'),'header':pin(a.output/'argument_plan.h'),'limits':plan['limits'],'serialized_bounds':plan['serialized_bounds']},indent=2))

if __name__=='__main__':main()
