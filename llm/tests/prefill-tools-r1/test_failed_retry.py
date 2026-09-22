"""Filesystem/CLI tests for immutable failed-run retry chains; no remote work."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('prefill_existing_tests',HERE/'test_tools.py')
F=importlib.util.module_from_spec(spec);spec.loader.exec_module(F)
P,S,Fsave=F.P,F.S,F.save
R=P.retry_chain

def invoke(root,admission):
    ap=Fsave(root/(admission['case_id']+'-admission.json'),admission)
    def preflight(command,**kwargs):
        if '--preflight-only' not in command:raise AssertionError('cache execution forbidden')
        out=Path(command[command.index('--output')+1])
        Fsave(out/'status.json',dict(status='PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT',
            input_preflight=dict(kernel_count=7,phases=P.phases(2)),fast_source_runtime_pins=[]))
        return types.SimpleNamespace(stdout='',stderr='',check_returncode=lambda:None)
    with patch.object(P,'REMOTE_ROOT',root),patch.object(P.subprocess,'run',side_effect=preflight),\
         patch.object(sys,'argv',['prepare','--root',str(root),'--admission',str(ap),'--cpu','8']),\
         contextlib.redirect_stdout(io.StringIO()):P.main()
    return root/'cases'/admission['case_id']/'prepare-result.json'

def setup(root,prefill=256):
    inp=root/'inputs';inp.mkdir();a=F.fixture(inp,prefill)
    producer=root/'repo/llm/executor-r1/whole_stream.py'
    producer.parent.mkdir(parents=True);producer.write_text('# synthetic no cache execution')
    original=invoke(root,a);fail(root,a['case_id'])
    return a,original

def fail(root,name,**changes):
    directory=root/'cases'/name;prepare=R.load(directory/'prepare-result.json')
    job=dict(status='FAIL_PROCESS',case_id=name+'-r4',gpu=None,spec=prepare['specs'][0]['spec'],
             process=dict(returncode=1,cleanup=dict(owned_descendants_empty=True)))
    job.update(changes);return Fsave(directory/'r4-job/job-finish.json',job)

def retry(root,a,parent=None,number=1):
    b=copy.deepcopy(a);parent=parent or a['case_id'];b['case_id']='qwen-p%dd2-retry-r%d'%(a['prefill_length'],number)
    directory=root/'cases'/parent
    b['supersedes_failed_case']=dict(prepare=R.pin(directory/'prepare-result.json'),
                                    job_finish=R.pin(directory/'r4-job/job-finish.json'))
    return b

def validate(root,a):
    paths,pins,_=P.validate_admission(a);_,_,_,specs=P.build_specs(root,a,paths,8,pins)
    return R.validate_new(root,a,P.pin(paths['graph']),R.load(paths['graph'])['input_contract'],
                          P.pin(paths['launch_resources']),specs['r4'])

class FailedRetryTests(unittest.TestCase):
    def test_failed_retry_cli_preserves_original_and_selects_new_terminal(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();a,original=setup(root)
            before={str(p):p.read_bytes()for p in original.parent.rglob('*')if p.is_file()}
            b=retry(root,a);new=invoke(root,b)
            self.assertEqual(before,{str(p):p.read_bytes()for p in original.parent.rglob('*')if p.is_file()})
            receipt=R.load(new);self.assertEqual(receipt['supersedes_failed_case'],b['supersedes_failed_case'])
            row=next(r for r in S.aggregate(root)['cases']if r['prefill_length']==256)
            self.assertEqual((row['case_id'],row['validated_failed_retry_count']),('qwen-p256d2-retry-r1',1))
            self.assertEqual(row['logical_case_id'],'qwen-p256d2')
            with self.assertRaises(ValueError):invoke(root,b)

    def test_reject_running_success_bad_exit_and_unclean_predecessor(self):
        for change in ('running','success','returncode','cleanup','wrong_job','wrong_spec'):
            with self.subTest(change=change),tempfile.TemporaryDirectory()as tmp:
                root=Path(tmp).resolve();a,_=setup(root);jp=root/'cases'/a['case_id']/'r4-job/job-finish.json';j=R.load(jp)
                if change=='running':j['status']='RUNNING'
                elif change=='success':j['status']='PASS_PROCESS_ONLY'
                elif change=='returncode':j['process']['returncode']=0
                elif change=='cleanup':j['process']['cleanup']['owned_descendants_empty']=False
                elif change=='wrong_job':j['case_id']='different-r4'
                else:j['spec']=P.pin(root/'inputs/graph.json')
                Fsave(jp,j)
                with self.assertRaises(ValueError):validate(root,retry(root,a))

    def test_missing_or_mutated_pinned_failure_rejected(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();a,_=setup(root);b=retry(root,a)
            jp=Path(b['supersedes_failed_case']['job_finish']['path']);j=R.load(jp);j['extra']='changed';Fsave(jp,j)
            with self.assertRaisesRegex(ValueError,'pin mismatch'):validate(root,b)
            jp.unlink()
            with self.assertRaises(FileNotFoundError):validate(root,b)

    def test_graph_resources_contract_and_fixed_environment_are_immutable(self):
        for field in ('graph','launch_resources','input_contract','old_env','new_env'):
            with self.subTest(field=field),tempfile.TemporaryDirectory()as tmp:
                root=Path(tmp).resolve();a,pp=setup(root);b=retry(root,a);old=R.load(pp)
                child=dict(old,case_id=b['case_id'],supersedes_failed_case=b['supersedes_failed_case'])
                if field in ('graph','launch_resources'):
                    cp=root/('copy-'+field+'.json');cp.write_bytes(Path(old[field]['path']).read_bytes());child[field]=R.pin(cp)
                elif field=='input_contract':child[field]=dict(child[field],decode_input_ids=[7,7])
                elif field=='old_env':
                    sp=Path(old['specs'][0]['spec']['path']);x=R.load(sp);x['environment']['TILEGEN_EF_HIT_RATE']='0';Fsave(sp,x)
                    old['specs'][0]['spec']=R.pin(sp);Fsave(pp,old);fail(root,a['case_id']);child['supersedes_failed_case']=retry(root,a)['supersedes_failed_case']
                else:
                    paths,pins,_=P.validate_admission(b);_,_,_,specs=P.build_specs(root,b,paths,8,pins)
                    specs['r4']['environment']['TILEGEN_ADA_SHARED_BYTES']='32768'
                    with self.assertRaises(ValueError):R.validate_new(root,b,old['graph'],old['input_contract'],old['launch_resources'],specs['r4'])
                    continue
                with self.assertRaises(ValueError):R.edge(pp,old,child)

    def test_status_rejects_unlinked_duplicate_fork_and_running_coverage(self):
        for change in ('no_link','fork','running_parent'):
            with self.subTest(change=change),tempfile.TemporaryDirectory()as tmp:
                root=Path(tmp).resolve();a,_=setup(root);b=retry(root,a);rp=invoke(root,b)
                if change=='no_link':
                    x=R.load(rp);del x['supersedes_failed_case'];Fsave(rp,x)
                elif change=='fork':
                    x=R.load(rp);x['case_id']='qwen-p256d2-retry-r2'
                    Fsave(root/'cases'/x['case_id']/'prepare-result.json',x)
                else:
                    jp=root/'cases'/a['case_id']/'r4-job/job-finish.json';j=R.load(jp);j['status']='RUNNING';Fsave(jp,j)
                    x=R.load(rp);x['supersedes_failed_case']['job_finish']=R.pin(jp);Fsave(rp,x)
                with self.assertRaises(ValueError):S.aggregate(root)

    def test_multihop_requires_latest_failed_tail(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();a,_=setup(root);b=retry(root,a);invoke(root,b)
            with self.assertRaises(FileNotFoundError):retry(root,a,parent=b['case_id'],number=2)
            fail(root,b['case_id']);c=retry(root,a,parent=b['case_id'],number=2);invoke(root,c)
            self.assertEqual(R.selected_preparations(root)[256][2],2)
            with self.assertRaises(ValueError):validate(root,retry(root,a,number=3))

    def test_retry_ncu_uses_logical_path_and_latest_contract(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();a,_=setup(root);b=retry(root,a);invoke(root,b)
            contract=R.load(a['graph'])['input_contract'];rows=[dict(roi=x,n=1,dram_read_bytes=128,dram_write_bytes=32)for x in ('full','Prefill','D1','D2')]
            path=root/'references/qwen-p256d2/ncu-result.json';Fsave(path,dict(status='PASS_CLOSED_RAW_NCU_FIXTURE',input_contract=contract,ROI_rows=rows,metadata_actual_controls_compared=True,independent_ROIs_are_additive=False))
            row=next(r for r in S.aggregate(root)['cases']if r['prefill_length']==256)
            self.assertEqual(row['NCU']['ROI_rows'],[dict(x,gpu_duration_ns=None)for x in rows])
            self.assertFalse((root/'references'/b['case_id']).exists())
            data=R.load(path);data['input_contract']=dict(contract,prefill_length=512);Fsave(path,data)
            self.assertEqual(S.ncu_reference(root,b['case_id'],256)['status'],'INPUT_CONTRACT_MISMATCH')

    def test_unlinked_new_retry_without_prior_case_is_rejected(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();inp=root/'inputs';inp.mkdir();a=F.fixture(inp,256);a['case_id']='qwen-p256d2-retry-r1'
            with self.assertRaises(ValueError):validate(root,a)

    def test_p512_retry_accepts_new_qualified_runner_without_changing_old_binary(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp).resolve();a,original=setup(root,512);old_binary=Path(a['runner']).read_bytes()
            b=retry(root,a);binary=root/'fixed-runner';binary.write_bytes(b'\x7fELFnew-qualified-fixture')
            witness=R.load(a['runner_evidence']['path']);qualification=R.load(witness['case_validation']['path'])
            qualification['runner_sha256']=P.pin(binary)['sha256'];qp=Fsave(root/'new-case-validation.json',qualification)
            witness.update(binary=P.pin(binary),case_validation=P.pin(qp));wp=Fsave(root/'new-runner-evidence.json',witness)
            b.update(runner=str(binary),runner_evidence=P.pin(wp));new=invoke(root,b)
            self.assertEqual(Path(a['runner']).read_bytes(),old_binary)
            self.assertNotEqual(R.load(original)['binary']['sha256'],R.load(new)['binary']['sha256'])
            terminal=R.selected_preparations(root)[512]
            self.assertEqual((terminal[1]['case_id'],terminal[2]),('qwen-p512d2-retry-r1',1))

if __name__=='__main__':unittest.main(verbosity=2)
