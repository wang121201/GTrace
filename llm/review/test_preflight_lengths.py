"""Bounded source-admission tests for the requested independent P128 cases."""
import json
from pathlib import Path
import tempfile
import types
import unittest

from test_producer_contract import load, NEW


class LengthAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load(NEW, 'p128_length_admission')

    def fixture(self, root, prefill, decode):
        process = dict(pid=7, start_ticks=8)
        names = [f'{stage}/{phase}' for stage in ('Warmup','Measured')
                 for phase in ['Prefill']+[f'Decode{i}' for i in range(1,decode+1)]]
        nodes, entries, records, arguments, phases = [], {}, {}, {}, []
        for i, phase in enumerate(names, 1):
            ident = f'kernel:{i}'
            begin, end = 3*i, 3*i+2
            nodes.append(dict(id=ident, kind='native_kernel', submission_event=3*i+1,
                 process=process, context_handle_u64=1,stream_u64=1,code_sha256='synthetic-code',
                 native_launch_id=i,function_id=1,argument_record_sha256='synthetic-argument'))
            entries[ident] = dict(node_id=ident, process=process,code_sha256='synthetic-code',
                 native_launch_id=i,function_id=1,graph_argument_sha256='synthetic-argument',
                 source_launch_key=f'epoch-{i}-launch-0',binding={},
                 global_program_complete_for_supported_specialization=True)
            nodes[-1].update(grid=[1,1,1], block=[32,1,1])
            arguments[ident] = dict(epoch_id=i,epoch_launch_ordinal=0,phase=phase)
            records[begin]=dict(epoch_id=i,event_ordinal=begin)
            records[end]=dict(epoch_id=i,event_ordinal=end)
            phases.append(dict(phase=phase,epoch_id=i,begin=begin,end=end))
        path = root/'nodes.jsonl'
        path.write_text(''.join(json.dumps(n)+'\n' for n in nodes))
        contract=dict(prefill_length=prefill,decode_steps=decode,batch_size=1,dtype='bfloat16',
                      warmup_runs=1,sampling_retained=True,output_feedback=False,cuda_graph=False)
        graph=types.SimpleNamespace(value=dict(input_contract=contract,process=process,
             artifacts=dict(nodes=path),phases=phases),argument=lambda k:arguments[k],record=lambda r:records[r])
        return graph, entries

    def test_requested_cases_require_all_warmup_and_measured_phases(self):
        for prefill,decode in [(32,2),(128,2),(128,4),(128,8),(128,16)]:
            with self.subTest(prefill=prefill,decode=decode), tempfile.TemporaryDirectory() as tmp:
                g,e=self.fixture(Path(tmp),prefill,decode)
                timeline,receipt=self.module.preflight(g,e)
                self.assertEqual(receipt['kernel_count'],2*(decode+1))
                self.assertEqual(len(receipt['phases']),2*(decode+1))
                self.assertEqual(len(timeline),6*(decode+1))

    def test_unrequested_lengths_rejected(self):
        for prefill,decode in [(32,4),(128,1),(128,3),(128,32),(256,2)]:
            with self.subTest(prefill=prefill,decode=decode), tempfile.TemporaryDirectory() as tmp:
                g,e=self.fixture(Path(tmp),prefill,decode)
                with self.assertRaisesRegex(ValueError,'only P32/D2 regression'):
                    self.module.preflight(g,e)

    def test_truncated_warmup_is_not_an_independent_shorter_case(self):
        with tempfile.TemporaryDirectory() as tmp:
            g,e=self.fixture(Path(tmp),128,4)
            g.value['phases']=[p for p in g.value['phases'] if p['phase']!='Warmup/Decode4']
            with self.assertRaisesRegex(ValueError,'complete contract-derived phase order'):
                self.module.preflight(g,e)


if __name__=='__main__':
    unittest.main(verbosity=2)
