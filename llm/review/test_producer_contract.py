"""Bounded synthetic protocol checks; no native inputs, cache binary, or GPU."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
NEW = HERE.parent / 'executor-r1/whole_stream.py'
BASE = HERE.parents[3]
OLD = BASE / 'write-gap-fix-20260920-r1/llama-p32d2-20260921/cpu-llama-package-r1/tree/llama-p32d2-20260921/executor-r1/whole_stream.py'

def pin(p):
    p = Path(p).resolve(); b = p.read_bytes()
    return dict(path=str(p), bytes=len(b), sha256=hashlib.sha256(b).hexdigest())

def load(path, name):
    stub = types.ModuleType('graph_io')
    stub.Graph = None; stub.checked = lambda p: Path(p); stub.pin = pin
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    before = list(sys.path)
    with patch.dict(sys.modules, {'graph_io': stub}):
        spec.loader.exec_module(module)
    sys.path[:] = before
    return module

class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old = load(OLD, 'old_producer_review')
        cls.new = load(NEW, 'new_producer_review')

    def fixture(self, schema='ordinary'):
        binding = dict(schema=schema, code_sha256='fixture-code', grid=[1,1,1])
        entry = dict(binding=binding, code_sha256='fixture-code', family='fixture', function_id=1)
        node = dict(id=3,kind='native_kernel',native_launch_id=7,phase='Measured/Decode1',module_scope='fixture',grid=[1,1,1],block=[32,1,1])
        allocation = dict(id=1,kind='allocation_API_observation',phase=None,history={'region':'initialization'},observation={'action':'allocate'})
        api = dict(id=2,kind='memory_api_submission',native_memory_operation_id=4,phase=None,history={'region':'initialization'},operation=dict(direction_known=True,device_address_extent_qualified=True,geometry='linear',height=1,depth=1,requested_bytes=40,width_bytes=40,action='memcpy',direction='host_to_device',cuda_api='synthetic_H2D',destination=dict(kind='device',address_u64=4096)))
        timeline = [(1,'node',allocation),(2,'node',api),(3,'phase_begin','Measured/Decode1'),(4,'node',node),(5,'phase_end','Measured/Decode1')]
        events = [dict(operation='READ',lane_addresses=[4096]*32,cta=0,warp=0,pc=16,width=4,effective_mask=3,role='weights'),dict(operation='WRITE',lane_addresses=[8192]*32,cta=0,warp=0,pc=32,width=4,effective_mask=1,role='output'),dict(operation='GLOBAL_TO_SHARED',lane_addresses=[12288]*32,cta=0,warp=0,pc=48,width=16,effective_mask=0xffffffff,source_read_mask=0,role='activation'),dict(kind='control',action='block_reduction_and_barrier')]
        statics=[dict(offset=16,opcode='LDG.E.EF.32',nvbit_size=4),dict(offset=32,opcode='STG.E.32',nvbit_size=4),dict(offset=48,opcode='LDGSTS.E.BYPASS.LTC128B.128',nvbit_size=16)]
        graph=types.SimpleNamespace(value={'input_contract':{'decode_steps':1}},function_static=lambda f:statics)
        registry=types.SimpleNamespace(events=lambda *a,**k:iter(events))
        return timeline, registry, {3:entry}, graph

    @staticmethod
    def normalized(rows):
        result=[]
        for r in rows:
            if r['type']=='allocation_metadata': continue
            r=dict(r);r.pop('observed_shared_bytes',None);result.append(r)
        return result

    def test_metadata_only_additions_preserve_memory_and_api_stream(self):
        args=self.fixture(); a=[];b=[]
        self.old.emit(*args[:3],a.append,lambda r:None,graph=args[3])
        self.new.emit(*args[:3],b.append,lambda r:None,graph=args[3],launch_resources={7:8192})
        self.assertEqual(a,self.normalized(b))
        self.assertEqual(sum(r['type']=='allocation_metadata' for r in b),1)
        self.assertEqual(next(r for r in b if r['type']=='begin_kernel')['observed_shared_bytes'],8192)
        source=[r for r in b if r['type']=='memory']
        self.assertEqual(len(source),3)
        self.assertEqual(source[2]['event']['global_effective_mask'],0)
        self.assertTrue(source[2]['policy']['bypass_l1'])
        self.assertEqual(next(r for r in b if r['type']=='api_range')['bytes'],40)

    def test_compact_gemv_and_conditional_provider_admission(self):
        for schema,selected in [('CURRENT_QWEN_GEMV_SOURCE_BINDING_V1','qwen-gemv-native-program-r1'),('CURRENT_LLAMA_GEMV_SOURCE_BINDING_V1','llama-gemv-native-program-r1')]:
            with self.subTest(schema=schema):
                args=self.fixture(schema);a=[];b=[];loads=[]
                provider=types.SimpleNamespace(RANGES={'fixture-code':None},EPILOG={'fixture-code':'fixture-epilog'},qualify_static=lambda code,static:['fixture-stage'])
                with patch.object(self.old,'load_module',return_value=provider):
                    self.old.emit(*args[:3],a.append,lambda r:None,graph=args[3],fast_gemv=True)
                with patch.object(self.new,'load_module',side_effect=lambda p,**k:(loads.append(Path(p).parent.name) or provider)):
                    self.new.emit(*args[:3],b.append,lambda r:None,graph=args[3],fast_gemv=True,launch_resources={7:16384})
                self.assertEqual(a,self.normalized(b));self.assertEqual(loads,[selected])
                self.assertEqual(sum(r['type']=='native_gemv_program' for r in b),1)

    def run_main_fixture(self, transform, expect_error, preflight=False):
        # Three launches represent initialization, warmup and measured history.
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('graph','registry','runtime','runner'):(root/name).write_text('{}')
            resources=dict(schema='OBSERVED_LAUNCH_CARVEOUT_V1',graph_sha256=pin(root/'graph')['sha256'],evidence_scope='per_kernel_launch',kernels=[dict(native_launch_id=i,observed_shared_bytes=s) for i,s in [(1,8192),(2,16384),(3,32768)]])
            transform(resources);(root/'resources').write_text(json.dumps(resources))
            graph=types.SimpleNamespace(close=lambda:None)
            registry=types.SimpleNamespace(entries={},close=lambda:None)
            timeline=[(i,'node',dict(kind='native_kernel',native_launch_id=i)) for i in (1,2,3)]
            receipt=dict(kernel_count=3,phases=['fixture-only'])
            argv=['producer','--graph',str(root/'graph'),'--registry',str(root/'registry'),'--runtime',str(root/'runtime'),'--runner',str(root/'runner'),'--output',str(root/'out'),'--launch-resources',str(root/'resources')]
            if preflight:argv+=['--preflight-only']
            with patch.object(sys,'argv',argv),patch.object(self.new,'Graph',return_value=graph),patch.object(self.new,'load_module',return_value=types.SimpleNamespace(Registry=lambda p:registry)),patch.object(self.new,'preflight',return_value=(timeline,receipt)),patch.object(self.new.subprocess,'Popen') as popen,contextlib.redirect_stdout(io.StringIO()):
                if expect_error:
                    with self.assertRaises((ValueError,KeyError)):self.new.main()
                else:self.new.main()
                popen.assert_not_called()
            if not expect_error:self.assertEqual(json.loads((root/'out/status.json').read_text())['status'],'PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT')

    def test_complete_three_history_regions_preflight(self):self.run_main_fixture(lambda x:None,False,True)
    def test_missing_initialization_rejected_before_cache(self):self.run_main_fixture(lambda x:x['kernels'].pop(0),True)
    def test_extra_launch_rejected_before_cache(self):self.run_main_fixture(lambda x:x['kernels'].append(dict(native_launch_id=4,observed_shared_bytes=32768)),True)
    def test_duplicate_launch_rejected_before_cache(self):self.run_main_fixture(lambda x:x['kernels'].append(x['kernels'][0]),True)
    def test_wrong_graph_rejected_before_cache(self):self.run_main_fixture(lambda x:x.update(graph_sha256='0'*64),True)
    def test_range_scope_rejected_before_cache(self):self.run_main_fixture(lambda x:x.update(evidence_scope='ROI'),True)
    def test_invalid_shared_rejected_before_cache(self):self.run_main_fixture(lambda x:x['kernels'][0].update(observed_shared_bytes=12288),True)

if __name__=='__main__':unittest.main(verbosity=2)
