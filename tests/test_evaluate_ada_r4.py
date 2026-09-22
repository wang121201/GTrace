"""Small synthetic checks only; this fixture is never an experimental result."""
import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('evaluate_ada_r4', Path(__file__).resolve().parents[1]/'tools/evaluate_ada_r4.py')
E = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(E)


def fixture():
    rows, hardware, frozen = [], {}, {m: {} for m in E.MODELS}
    for cid in range(384):
        shared = (32768,65536,102400)[cid%3]
        nominal = E.CAPACITY[shared][0]
        cg = 360 <= cid < 372
        misses = 6 if cg else 3
        metric = {'shared_bytes': shared, 'l2_reads': misses}
        hardware[str(cid)] = {'case': {'id':cid,'cg':cg,'scalar_loads':6,'y':32,'z':0},
            'median':metric, 'repeats':[metric.copy() for _ in range(5)],'read_group_stable':True}
        for i, model in enumerate(E.MODELS):
            cap = E.CAPACITY[shared][i]
            sets = 16 if i else 4
            rows.append(dict(schema='ADA_R4_SHARED_SERIAL_CASE_V1',status='PASS',case_id=cid,
                model_id=model, observed_shared_bytes=shared, nominal_l1_bytes=nominal,
                capacity_bytes=cap,sets=sets,ways=cap//128//sets,policy='CLOCK' if i else 'LRU',
                allocation_unit=128,sector_bytes=32,hash=2 if i else 0,scale=1062 if i else 1000,
                hits=6-misses,misses=misses,requests=6,expected_requests=6,L2_read_sectors=misses,
                cg=cg,CPU_seconds=.06,wall_seconds=.12))
            frozen[model][str(cid)] = dict(hits=6-misses,misses=misses,capacity_bytes=cap)
    return rows, hardware, frozen


class EvaluationTests(unittest.TestCase):
    def test_receipt_closed(self):
        receipt=dict(schema='GTSIM_ADA_R4_SERIAL_RECEIPT_V1',status='PASS_PROCESS_ONLY',
            source_unchanged=True,input_unchanged=True,case_count=384,returncode=0,
            GPU_executed=False,prediction_sha256='a'*64)
        self.assertEqual(E.validate_replay_receipt(receipt,'a'*64)['case_count'],384)

    def test_receipt_each_required_guard(self):
        receipt=dict(schema='GTSIM_ADA_R4_SERIAL_RECEIPT_V1',status='PASS_PROCESS_ONLY',
            source_unchanged=True,input_unchanged=True,case_count=384,returncode=0,
            GPU_executed=False,prediction_sha256='a'*64)
        for field,bad in [('schema','unknown'),('status','FAILED'),('status','RUNNING'),
                ('source_unchanged',False),('input_unchanged',False),('source_unchanged',1),
                ('case_count',383),('returncode',1),('returncode',False),
                ('GPU_executed',True),('GPU_executed',0),('prediction_sha256','b'*64)]:
            with self.subTest(field=field,bad=bad),self.assertRaises(ValueError):
                E.validate_replay_receipt({**receipt,field:bad},'a'*64)
        for field in receipt:
            invalid=receipt.copy();invalid.pop(field)
            with self.subTest(missing=field),self.assertRaises(ValueError):
                E.validate_replay_receipt(invalid,'a'*64)

    def test_completed_predictions_do_not_override_failed_wrapper(self):
        rows,hardware,frozen=fixture()
        self.assertEqual(E.evaluate(rows,hardware,frozen)[0]['status'],'PASS_REFERENCE_EQUIVALENCE')
        receipt=dict(schema='GTSIM_ADA_R4_SERIAL_RECEIPT_V1',status='FAILED',
            source_unchanged=False,input_unchanged=True,case_count=384,returncode=0,
            GPU_executed=False,prediction_sha256='a'*64)
        with self.assertRaises(ValueError):E.validate_replay_receipt(receipt,'a'*64)

    def test_full_scope_and_units(self):
        summary, derived = E.evaluate(*fixture())
        self.assertEqual(summary['status'],'PASS_REFERENCE_EQUIVALENCE')
        self.assertEqual(len(derived),768)
        for model in summary['models'].values():
            self.assertEqual(model['fresh_ca']['conditions'],360)
            self.assertEqual(model['fresh_ca']['WAPE_percent'],0)
            self.assertAlmostEqual(model['all_cases']['replay_CPU_minutes'],.384)
            self.assertEqual(model['cg_controls']['conditions'],12)
            self.assertEqual(model['cross_time_anchors']['conditions'],12)

    def test_absolute_error_not_signed_cancellation(self):
        rows = [dict(L2_read_sectors=p,NCU_median_L2_read_sectors=10,
                frozen_prediction_equal=True,CPU_seconds=0,wall_seconds=0) for p in (5,15)]
        result = E.aggregate(rows)
        self.assertEqual(result['signed_aggregate_error_percent'],0)
        self.assertEqual(result['WAPE_percent'],50)

    def test_missing_and_duplicate_reject(self):
        r,h,f=fixture()
        with self.assertRaises(ValueError):E.evaluate(r[:-1],h,f)
        with self.assertRaises(ValueError):E.evaluate(r+[r[0]],h,f)

    def test_observed_mismatch_reject(self):
        r,h,f=fixture();h['0']['repeats'][1]['shared_bytes']=65536
        with self.assertRaises(ValueError):E.evaluate(r,h,f)

    def test_ncumedian_mismatch_reject(self):
        r,h,f=fixture();h['0']['median']['l2_reads']=99
        with self.assertRaises(ValueError):E.evaluate(r,h,f)

    def test_static_capacity_mismatch_reject(self):
        r,h,f=fixture();r[1]['capacity_bytes']=98304
        with self.assertRaises(ValueError):E.evaluate(r,h,f)

    def test_reference_mismatch_is_failure_not_hardware_fit(self):
        r,h,f=fixture();r[0].update(hits=2,misses=4,L2_read_sectors=4)
        summary,_=E.evaluate(r,h,f)
        self.assertEqual(summary['status'],'FAIL_REFERENCE_EQUIVALENCE')
        self.assertEqual(summary['frozen_prediction_mismatches'],1)

    def test_source_count_mismatch_reject(self):
        r,h,f=fixture();h['0']['case']['scalar_loads']=7
        with self.assertRaises(ValueError):E.evaluate(r,h,f)

    def test_zero_reference_is_null_not_zero_error(self):
        row=dict(L2_read_sectors=4,NCU_median_L2_read_sectors=0,
            frozen_prediction_equal=True,CPU_seconds=0,wall_seconds=0)
        result=E.aggregate([row])
        self.assertIsNone(result['WAPE_percent'])
        self.assertIsNone(result['signed_aggregate_error_percent'])


if __name__=='__main__':unittest.main()
