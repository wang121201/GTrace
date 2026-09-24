"""Check diagnostic isolation without importing remote native source programs."""
import ast
import collections
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import unittest

path=Path(__file__).resolve().parents[1]/'executor-r1/whole_stream.py'
tree=ast.parse(path.read_text())
ns=dict(collections=collections,hashlib=hashlib,json=json)
names={'need','Writer','diagnostic_drain','phase_traffic','emit'}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n,(ast.FunctionDef,ast.ClassDef)) and n.name in names],type_ignores=[]),str(path),'exec'),ns)

class Boundaries(unittest.TestCase):
    def stream(self,policy):
        phases=['Warmup/Prefill','Warmup/Decode1','Warmup/Decode2','Measured/Prefill','Measured/Decode1','Measured/Decode2']
        timeline=[(i,'phase_'+e,p) for i,(p,e) in enumerate((p,e) for p in phases for e in ('begin','end'))]
        rows=[]; stream=io.BytesIO(); writer=ns['Writer'](stream)
        def send(r):rows.append(r);writer(r)
        ns['emit'](timeline,None,{},send,lambda r:None,graph=SimpleNamespace(value={'input_contract':{'decode_steps':2}}),drain_policy=policy)
        return rows,writer
    def test_natural_source_unchanged(self):
        original,w=self.stream('none')
        self.assertEqual(w.hash.hexdigest(),w.source_hash.hexdigest())
        for policy in ('run-end','measured-phase-end'):
            rows,intervened=self.stream(policy)
            self.assertEqual(intervened.source_hash.hexdigest(),w.hash.hexdigest())
            stripped=[r for r in rows if r['type']!='drain' and not(r['type']=='snapshot' and '/diagnostic-drain/' in r['label'])]
            self.assertEqual(stripped,original)
    def test_drain_after_natural_end(self):
        rows,_=self.stream('measured-phase-end')
        for i,r in enumerate(rows):
            if r['type']=='drain':
                natural=next(j for j,s in enumerate(rows) if s.get('label')==r['label']+'/end')
                self.assertLess(natural,i)
        finalend=next(i for i,r in enumerate(rows) if r.get('label')=='Measured/Full/end')
        lastdrain=max(i for i,r in enumerate(rows) if r['type']=='drain')
        self.assertLess(finalend,lastdrain)
    def test_runend_drain_outside_all_natural_ranges(self):
        rows,_=self.stream('run-end')
        i=next(i for i,r in enumerate(rows) if r['type']=='drain')
        self.assertTrue(all(j<i for j,r in enumerate(rows) if r['type']=='snapshot' and '/diagnostic-drain/' not in r['label']))
        self.assertEqual(rows[-1]['type'],'run_end')
    def test_dirty_conservation_and_typed_read_rejection(self):
        b=dict(DRAM_read_bytes=0,DRAM_write_bytes=0,source_read_effect_bytes=0,source_write_effect_bytes=0,dirty_sector_creations=0,evicted_dirty_sectors=0,age_writeback_bytes=0,capacity_eviction_writeback_bytes=0,L2_forwarded_access_sequence=0,dirty_tail_bytes=32)
        e=dict(b,DRAM_write_bytes=32,dirty_tail_bytes=0)
        row=ns['phase_traffic']('Decode1',{'cumulative':b},{'cumulative':e})
        self.assertEqual(row['dirty_start_bytes'],32);self.assertEqual(row['DRAM_write_bytes'],32)
        with self.assertRaisesRegex(ValueError,'dirty ledger'):
            ns['phase_traffic']('Decode1',{'cumulative':b},{'cumulative':dict(e,dirty_tail_bytes=32)})
        causes=('load_fill','read_merge','atomic_read','capacity_merge','age_merge','drain_merge','old_store_RFO','old_atomic_RFO')
        for c in causes:b['DRAM_'+c+'_bytes']=0;e['DRAM_'+c+'_bytes']=0
        e['DRAM_read_bytes']=32
        with self.assertRaisesRegex(ValueError,'typed DRAM reads'):
            ns['phase_traffic']('Decode1',{'cumulative':b},{'cumulative':e})

if __name__=='__main__':unittest.main()
