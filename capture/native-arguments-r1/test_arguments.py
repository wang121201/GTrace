#!/usr/bin/env python3
"""Small synthetic CPU-only consumer rejection fixtures; no GPU or calibration."""
import collections
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import consumer as C
import make_argument_plan as P


def raw(value):return (json.dumps(value,separators=(',',':'))+'\n').encode()
def sha(value):return hashlib.sha256(value).hexdigest()
def write(path,value):path.write_bytes(raw(value))


class Fixture:
    def __init__(self,root):
        self.root=Path(root);self.step=self.root/'native';self.art=self.step/'artifacts';self.obs=self.root/'observer/process-200-300'
        self.art.mkdir(parents=True);self.obs.mkdir(parents=True)
        frozen=C.load_r1();self.contract={'sha256':'a'*64,'phases':['Prefill','Decode1']}
        self.process={'pid':200,'start_ticks':300};self.modules=[];self.scopes=[];self.launches=[];self.args=[]
        self.ins={'pc':0,'sass':'EXIT;'};canonical=raw(self.ins);self.code=sha(canonical)
        self.functions=[{'function_id':9,'instruction_count':1,'code_sha256':self.code,'argument_sizes':[4,8]}]
        self.static=[{'schema':'sg_nvbit_static_instruction_v1','function_id':9,'instruction':self.ins}]
        # One initial unmeasured launch; new runs may change this count/ID offset.
        b=self.base(0,0,1);b.update(scope_bound=False,role='initialization',phase='',forward_id=-1,call_id=0,module_scope='',layer_id=-1)
        self.launches += [b,dict(b,edge='return',event_ordinal=2,cuda_status=0)]
        for e,phase in enumerate(self.contract['phases'],1):
            start=e*100;self.scopes += [dict(type='epoch_begin',epoch_id=e,event_ordinal=start,**self.process),dict(type='epoch_end',epoch_id=e,event_ordinal=start+90,**self.process)]
            parent=e*10;child=parent+1
            self.modules += [dict(call_id=parent,parent_call_id=None,phase=phase,module='model.layers.1',module_class='sglang.LlamaDecoderLayer'),dict(call_id=child,parent_call_id=parent,phase=phase,module='model.layers.0.self_attn.rotary_emb',module_class='sglang.Rotary')]
            for o in range(2):
                lid=1+(e-1)*2+o;b=self.base(lid,e,start+10+o*10)
                if o: b.update(call_id=child,module_scope='model.layers.0.self_attn.rotary_emb',layer_id=1)
                self.launches += [b,dict(b,edge='return',event_ordinal=b['event_ordinal']+1,cuda_status=0)]
        reference=json.loads((frozen.PACKAGE/'native-source-reference.json').read_text())
        self.manifest=dict(status='COMPLETE',input_contract=self.contract,native_source_unchanged=True,native_source_files=reference['files'],
            coverage=dict(native_scope_abi_enabled=True,kernel_launch_metadata=False),stage_files=['Prefill.json','Decode1.json'],process=self.process,
            driver_sha256=C.sha_file(frozen.PACKAGE/'sglang_driver.py'))
        self.finish=dict(status='PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE',**self.process,launch_error_count=0,unsupported_dispatch_count=0,graph_node_callback_count=0,unknown_launch_attribute_count=0,open_context_count=0,active_epoch=0,internal_inspection_dispatch_count=0,errors=[],epoch_begin_count=2,epoch_end_count=2,launch_before_count=5,launch_return_count=5,function_count=1,
            dynamic_instrumentation=False,memory_addresses_captured=False,actual_sm_placement_captured=False,device_synchronization_inserted=False,gpu_memory_allocated_by_observer=False,template_or_gtsim_admission=False,kernel_argument_values_captured=False)
        self.sync(False)
        census,_,_,before,_,modules=C.validate_metadata(self.step,self.contract,self.root)
        entries,aliases=C.enriched_calls(census,before,modules)
        # Reference IDs differ from all capture IDs. Only equivalence structure,
        # not raw context/function/module IDs or initial launch offsets, is frozen.
        for entry in entries:
            entry['native_launch_id']+=100;entry['function_id']+=100;entry['context_id']+=100
            if entry['module_scope']!='<phase-global>':entry['call_id']+=100
        bounds=[P.row_bound(p) for p in entries]
        self.plan=dict(schema=P.SCHEMA,raw_argument_values_captured=False,input_contract_sha256=self.contract['sha256'],phases=self.contract['phases'],phase_counts={'Prefill':2,'Decode1':2},cuda_api_counts={'cuLaunchKernel':4},source_metadata_bytes_before_finish=self.finish['metadata_bytes_before_finish'],shared_module_alias_launches=aliases,
            limits=dict(launches=4,arguments=8,raw_bytes=48,max_arguments=2,max_argument_bytes=8,max_launch_bytes=12,max_row_bytes=16384,max_file_bytes=65536),serialized_bounds=dict(maximum_row_bytes_including_newline=max(bounds),total_bytes_including_newlines=sum(bounds),method='exact SG_NATIVE_ARGUMENT_VECTOR_V1 serializer envelope; fresh uint64 IDs use 20 digits; ASCII plan strings; fixed-width hex and SHA'),launches=entries)
        self.plan_path=self.root/'argument-plan.json';write(self.plan_path,self.plan)
        for seq,call in enumerate(census['calls']):
            b=before[call['native_launch_id']]
            a=dict(schema='SG_NATIVE_ARGUMENT_VECTOR_V1',sequence=seq,native_launch_binding=dict(process=self.process,native_launch_id=b['launch_id'],source_launch_key=call['source_launch_key']),source_launch_key=call['source_launch_key'],epoch_id=b['epoch_id'],epoch_launch_ordinal=call['epoch_launch_ordinal'],forward_id=b['forward_id'],phase=b['phase'],cuda_api=b['cuda_api'],layer=b['layer_id'],module_scope=b['module_scope'],module_call_id=b['call_id'],module_kernel_ordinal=0,context_id=b['context_id'],function_id=b['function_id'],stream_u64=0,
                **{k:b[k] for k in ['code_sha256','code_sha256_kind','parameter_layout_sha256','grid','block','dynamic_shared_bytes','static_shared_bytes','registers','local_bytes_per_thread','launch_attributes']},argument_transport='kernelParams',capture_before_original_launch=True,device_memory_dereferenced=False,
                arguments=[dict(index=i,size_bytes=n,parameter_buffer_offset=None,raw_bytes_hex=bytes([seq+i]*n).hex(),sha256=sha(bytes([seq+i]*n))) for i,n in enumerate([4,8])])
            self.args.append(a)
        self.finish.update(status='PASS_NATIVE_ARGUMENT_OBSERVER_CLOSED_NOT_TRACE',kernel_argument_values_captured=True,argument_sideband_entries=4,argument_sideband_returns=4,argument_sideband_argument_count=8,argument_sideband_raw_bytes=48,argument_sideband_closed=True)
        self.sync()

    def base(self,lid,e,event):
        return dict(pid=200,start_ticks=300,launch_id=lid,edge='before',event_ordinal=event,monotonic_ns=event,epoch_id=e,phase=self.contract['phases'][e-1] if e else '',forward_id=e-1,role='measurement',cuda_api='cuLaunchKernel',scope_bound=True,metadata_supported=True,function_id=9,function_name='fixture_kernel',code_sha256=self.code,code_sha256_kind=P.KINDS,argument_sizes=[4,8],parameter_layout_sha256=sha(b'[4,8]'),context_id=7,stream_u64=0,module_scope='<phase-global>',call_id=10000000+e-1,layer_id=-1,grid=[1,1,1],block=[32,1,1],static_shared_bytes=0,dynamic_shared_bytes=0,registers=8,local_bytes_per_thread=0,launch_attributes=[],parameter_values_captured=False)

    def sync(self,arguments=True):
        write(self.art/'manifest.json',self.manifest);write(self.art/'module_calls.json',self.modules);write(self.art/'tensor_roots.json',[])
        for phase in self.contract['phases']:write(self.art/(phase+'.json'),{'phase':phase})
        write(self.art/'files.sha256.json',[dict(path=p.name,bytes=p.stat().st_size,sha256=C.sha_file(p)) for p in sorted(self.art.iterdir()) if p.name!='files.sha256.json'])
        if arguments:
            byid={a['native_launch_binding']['native_launch_id']:(i,a) for i,a in enumerate(self.args)}
            for b in self.launches:
                if b['launch_id'] in byid:
                    i,a=byid[b['launch_id']];b['parameter_values_captured']=True;b['native_argument_record']=dict(file='launch-arguments.jsonl',sequence=i,payload_sha256=sha(raw(a)[:-1]))
            self.finish['argument_sideband_plan_sha256']=C.sha_file(self.plan_path)
        datasets={'launch-journal.jsonl':self.launches,'scope-journal.jsonl':self.scopes,'functions.jsonl':self.functions,'static-instructions.jsonl':self.static,'lifecycle.jsonl':[],'allocation-journal.jsonl':[]}
        if arguments:datasets['launch-arguments.jsonl']=self.args
        for name,values in datasets.items():(self.obs/name).write_bytes(b''.join(raw(v) for v in values))
        self.seal_files()

    def seal_files(self):
        self.finish['files']=[dict(name=p.name,bytes=p.stat().st_size,sha256=C.sha_file(p)) for p in sorted(self.obs.glob('*.jsonl'))]
        self.finish['metadata_bytes_before_finish']=sum(x['bytes'] for x in self.finish['files'])
        write(self.obs/'finish.json',self.finish)

    def validate(self):return C.validate_arguments(self.step,self.contract,self.root,self.plan_path)


class ArgumentTests(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.f=Fixture(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def fails(self,mutate,message=None):
        mutate(self.f);self.f.sync()
        with self.assertRaises((ValueError,RuntimeError,KeyError,TypeError)) as caught:self.f.validate()
        if message:self.assertIn(message,str(caught.exception))
    def test_valid_rebinding_and_aliases(self):
        out=self.f.validate();self.assertEqual(out['status'],'PASS_NATIVE_ARGUMENT_PAYLOADS_ONLY');self.assertEqual((out['measured_launches'],out['argument_count'],out['argument_raw_bytes']),(4,8,48));self.assertEqual(out['shared_module_alias_launches'],2);self.assertEqual(len(out['journals']),7);self.assertEqual(out['calls'][1]['layer_id'],1)
    def test_all_actual_raw_and_source_fields(self):
        changes={'phase':'wrong','forward_id':True,'code_sha256':'f'*64,'code_sha256_kind':'cubin','parameter_layout_sha256':'b'*64,'grid':[2,1,1],'block':[64,1,1],'static_shared_bytes':1,'dynamic_shared_bytes':1,'registers':9,'local_bytes_per_thread':8,'launch_attributes':[{'id':6,'value':1}], 'cuda_api':'cuLaunchKernelEx','epoch_launch_ordinal':1,'module_kernel_ordinal':2,'module_call_id':999,'context_id':99,'function_id':99,'stream_u64':1,'layer':0,'sequence':True,'source_launch_key':'epoch-1-launch-9','argument_transport':'extra','capture_before_original_launch':False,'device_memory_dereferenced':True}
        for field,value in changes.items():
            with self.subTest(field=field):
                old=copy.deepcopy(self.f.args[0]);self.f.args[0][field]=value;self.f.sync()
                with self.assertRaises((ValueError,RuntimeError,KeyError,TypeError)):self.f.validate()
                self.f.args[0]=old
    def test_corrupt_payload(self):self.fails(lambda f:f.args[0]['arguments'][0].update(raw_bytes_hex='ff'*4))
    def test_bad_hex(self):self.fails(lambda f:f.args[0]['arguments'][0].update(raw_bytes_hex='GG'*4))
    def test_bad_width(self):self.fails(lambda f:f.args[0]['arguments'][0].update(size_bytes=5))
    def test_bad_hash(self):self.fails(lambda f:f.args[0]['arguments'][0].update(sha256='f'*64))
    def test_guessed_offset(self):self.fails(lambda f:f.args[0]['arguments'][0].update(parameter_buffer_offset=0))
    def test_missing_vector(self):self.fails(lambda f:f.args.pop())
    def test_extra_vector(self):self.fails(lambda f:f.args.append(copy.deepcopy(f.args[-1])))
    def test_reordered_vectors(self):self.fails(lambda f:f.args.reverse())
    def test_failed_return(self):self.fails(lambda f:f.launches[-1].update(cuda_status=1))
    def test_unmarked_enclosed_launch(self):
        self.fails(lambda f:[r.update(epoch_id=0) for r in f.launches if r['launch_id']==1], 'missing/wrong epoch')
    def test_duplicate_launch(self):self.fails(lambda f:f.launches.append(copy.deepcopy(f.launches[-1])))
    def test_missing_return(self):self.fails(lambda f:f.launches.pop())
    def test_module_parent_cycle(self):self.fails(lambda f:f.modules[1].update(parent_call_id=f.modules[1]['call_id']))
    def test_module_ancestry_drift(self):self.fails(lambda f:f.modules[0].update(module='model.layers.2'))
    def test_source_sha_drift(self):self.fails(lambda f:f.manifest['native_source_files'][0].update(sha256='0'*64))
    def test_driver_sha_drift(self):self.fails(lambda f:f.manifest.update(driver_sha256='0'*64))
    def test_bad_finish_counter(self):self.fails(lambda f:f.finish.update(argument_sideband_raw_bytes=49))
    def test_boolean_native_identity(self):self.fails(lambda f:[r.update(function_id=True) for r in f.launches if r['epoch_id']])
    def test_boolean_scope_flag(self):self.fails(lambda f:f.manifest['coverage'].update(native_scope_abi_enabled=1))
    def test_boolean_return_status(self):self.fails(lambda f:f.launches[-1].update(cuda_status=False))
    def test_old_finish_rejected(self):self.fails(lambda f:f.finish.update(status='PASS_METADATA_OBSERVER_CLOSED_NOT_TRACE'))
    def test_bad_ref_after_seal(self):
        self.f.launches[2]['native_argument_record']['payload_sha256']='f'*64
        (self.f.obs/'launch-journal.jsonl').write_bytes(b''.join(raw(x) for x in self.f.launches));self.f.seal_files()
        with self.assertRaises(RuntimeError):self.f.validate()
    def test_journal_hash_corruption(self):
        p=self.f.obs/'launch-arguments.jsonl';p.write_bytes(p.read_bytes()+b' ')
        with self.assertRaises(RuntimeError):self.f.validate()
    def test_duplicate_json_key(self):
        p=self.f.obs/'launch-arguments.jsonl';p.write_bytes(p.read_bytes().replace(b'"sequence":0',b'"sequence":0,"sequence":0',1));self.f.seal_files()
        with self.assertRaises(RuntimeError):self.f.validate()
    def test_missing_final_newline(self):
        p=self.f.obs/'launch-arguments.jsonl';p.write_bytes(p.read_bytes()[:-1]);self.f.seal_files()
        with self.assertRaises(RuntimeError):self.f.validate()
    def test_native_nondefault_stream(self):self.fails(lambda f:[r.update(stream_u64=1) for r in f.launches if r['epoch_id']])
    def test_plan_geometry_drift(self):
        self.f.plan['launches'][0]['grid']=[2,1,1];write(self.f.plan_path,self.f.plan)
        self.fails(lambda f:None)
    def test_plan_opaque_attribute_reject(self):
        plan=copy.deepcopy(self.f.plan);plan['launches'][0]['launch_attributes']=[{'event':999}]
        with self.assertRaises(RuntimeError):P.validate_plan(plan)
    def test_plan_negative_width(self):
        plan=copy.deepcopy(self.f.plan);plan['launches'][0]['argument_sizes']=[-1,8]
        with self.assertRaises(RuntimeError):P.validate_plan(plan)
    def test_plan_boolean_width(self):
        plan=copy.deepcopy(self.f.plan);plan['launches'][0]['argument_sizes']=[True,8]
        with self.assertRaises(RuntimeError):P.validate_plan(plan)
    def test_plan_aggregate_overflow(self):
        plan=copy.deepcopy(self.f.plan);plan['limits']['raw_bytes']=1<<64
        with self.assertRaises(RuntimeError):P.validate_plan(plan)
    def test_plan_row_file_budget_boundaries(self):
        plan=copy.deepcopy(self.f.plan);plan['limits']['max_row_bytes']=plan['serialized_bounds']['maximum_row_bytes_including_newline'];plan['limits']['max_file_bytes']=plan['serialized_bounds']['total_bytes_including_newlines'];P.validate_plan(plan)
        for field in ['max_row_bytes','max_file_bytes']:
            bad=copy.deepcopy(plan);bad['limits'][field]-=1
            with self.assertRaises(RuntimeError):P.validate_plan(bad)
    def test_plan_header_embeds_raw_sha(self):
        data=raw(self.f.plan);header=P.compile_header(data);self.assertIn(sha(data),header);self.assertIn('l.launches=4ull;',header)


if __name__=='__main__':unittest.main(verbosity=2)
