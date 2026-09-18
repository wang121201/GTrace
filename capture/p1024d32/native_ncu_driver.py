#!/usr/bin/env python3
"""Parameterized native SGLang workflow with one hardware ROI, no memory trace."""
import argparse
import contextlib
import ctypes
import dataclasses
import importlib.metadata
import json
import os
from pathlib import Path
import signal
import time

import sglang_driver as reference
import workload

SCOPES = ('full', 'Prefill', 'Decode1', 'Decode8', 'Decode16', 'Decode32')


class Boundary:
    def __init__(self, scope, phases, start, stop, journal=lambda event: None):
        if scope != 'full' and scope not in phases:
            raise ValueError('unsupported ROI')
        self.phases = phases
        self.scope, self.start_api, self.stop_api = scope, start, stop
        self.events = []
        self.active = False
        self.possible = False
        self.journal = journal

    def before(self, phase):
        if phase == (self.phases[0] if self.scope == 'full' else self.scope):
            if self.active or self.events:
                raise ValueError('duplicate profiler start')
            self.possible = True
            self.journal({'api': 'cudaProfilerStart', 'state': 'before', 'phase': phase})
            self.start_api()
            self.journal({'api': 'cudaProfilerStart', 'state': 'returned', 'phase': phase})
            self.active = True
            self.events.append({'api': 'cudaProfilerStart', 'before_phase': phase})

    def after(self, phase):
        if phase == (self.phases[-1] if self.scope == 'full' else self.scope):
            if not self.active:
                raise ValueError('profiler stop without start')
            self.journal({'api': 'cudaProfilerStop', 'state': 'before', 'phase': phase})
            self.stop_api()
            self.journal({'api': 'cudaProfilerStop', 'state': 'returned', 'phase': phase})
            self.active = False
            self.possible = False
            self.events.append({'api': 'cudaProfilerStop', 'after_phase': phase})

    def check(self):
        if self.active or len(self.events) != 2:
            raise ValueError('one complete profiler pair required')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--roi', choices=SCOPES, required=True)
    p.add_argument('--mode', choices=('capture', 'validate'), required=True)
    workload.add_arguments(p)
    a = p.parse_args()
    frozen = workload.from_args(a)
    if a.roi != 'full' and a.roi not in frozen['phases']:
        p.error('ROI outside frozen workload')
    a.output.mkdir(parents=True, exist_ok=True)
    out = a.output / ('process-%d' % os.getpid())
    out.mkdir(exist_ok=False)
    parent = os.getppid()
    if parent <= 1 or ctypes.CDLL(None).prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
        raise RuntimeError('owned parent-death guard unavailable')
    if os.getppid() != parent:
        raise RuntimeError('parent changed during guard setup')
    def journal(event):
        with (out / 'boundary-journal.jsonl').open('a') as f:
            f.write(json.dumps(dict(event, pid=os.getpid(), monotonic_ns=time.monotonic_ns())) + '\n')
    import torch
    import sglang
    import sglang.bench_one_batch as bo
    workload.check_packages()
    root = Path(sglang.__file__).resolve().parent
    source_before = reference.native_source_inventory(root)
    workload.check_native_sources(source_before)
    args = bo.ServerArgs(
        model_path='/home/xmu/.cache/modelscope/hub/models/LLM-Research/Meta-Llama-3-8B-Instruct',
        dtype='bfloat16', load_format='safetensors', device='cuda', tp_size=1, pp_size=1,
        attention_backend='flashinfer', disable_cuda_graph=True, cuda_graph_max_bs=1,
        enable_torch_compile=False, disable_overlap_schedule=True, disable_radix_cache=True,
        mem_fraction_static=0.90, max_total_tokens=frozen['max_total_tokens'], max_running_requests=1,
        random_seed=0, cpu_offload_gb=0)
    bo._set_envs_and_config(args)
    runner, _ = bo.load_model(args, bo.PortArgs.init_new(args), 0)
    if runner.cuda_graph_runner is not None or type(runner.model).__name__ != 'LlamaForCausalLM':
        raise RuntimeError('native eager model changed')
    if runner.model_config.hf_config.num_hidden_layers != 32:
        raise RuntimeError('full 32-layer model required')
    fixed = [torch.tensor([x], dtype=torch.int64, device=runner.device) for x in frozen['decode_input_ids']]
    with torch.no_grad():
        runner.req_to_token_pool.clear()
        runner.token_to_kv_pool_allocator.clear()
        _, _, batch = bo.extend(reference.make_request(bo, frozen), runner)
        for token in fixed:
            bo.decode(token, batch, runner)
        torch.cuda.synchronize()
        runner.req_to_token_pool.clear()
        runner.token_to_kv_pool_allocator.clear()

    # Capture only existing small control-tensor references; no GPU clone or
    # .cpu() is issued inside any measured range. seq_lens mutates in place,
    # so record the existing Python seq_lens_sum at each forward instead.
    controls = []
    current = None
    original = runner.forward

    def forward(fb, *fargs, **kwargs):
        record = {'phase': current, 'seq_lens_sum': int(fb.seq_lens_sum),
                  'input_ids_ref': fb.input_ids, 'positions_ref': fb.positions,
                  'cache_loc_ref': fb.out_cache_loc}
        result = original(fb, *fargs, **kwargs)
        if bool(result[1]):
            raise RuntimeError('unexpected CUDA Graph execution')
        controls.append(record)
        return result

    runner.forward = forward
    boundary = Boundary(a.roi, frozen['phases'],
                        torch.cuda.profiler.start if a.mode == 'capture' else lambda: None,
                        torch.cuda.profiler.stop if a.mode == 'capture' else lambda: None,
                        journal if a.mode == 'capture' else lambda event: None)
    events = [(torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True))
              for _ in frozen['phases']] if a.mode == 'validate' else []
    completed = []
    try:
        with torch.no_grad():
            batch = None
            for i, current in enumerate(frozen['phases']):
                torch.cuda.synchronize()
                boundary.before(current)
                if events:
                    events[i][0].record()
                with torch.profiler.record_function('phase/' + current):
                    torch.cuda.nvtx.range_push('phase/' + current)
                    try:
                        if i == 0:
                            predicted, logits, batch = bo.extend(reference.make_request(bo, frozen), runner)
                        else:
                            predicted, logits = bo.decode(fixed[i - 1], batch, runner)
                    finally:
                        torch.cuda.nvtx.range_pop()
                if events:
                    events[i][1].record()
                torch.cuda.synchronize()
                boundary.after(current)
                completed.append(current)
        boundary.check()
    finally:
        runner.forward = original
        if boundary.possible and a.mode == 'capture':
            torch.cuda.profiler.stop()
    if len(controls) != len(frozen['phases']) or completed != frozen['phases']:
        raise RuntimeError('natural frozen phase sequence incomplete')
    values = []
    for i, row in enumerate(controls):
        value = {k: v for k, v in row.items() if not k.endswith('_ref')}
        value.update(input_ids=row['input_ids_ref'].cpu().tolist(),
                     positions=row['positions_ref'].cpu().tolist(),
                     out_cache_loc=row['cache_loc_ref'].cpu().tolist())
        workload.validate_controls(frozen, i, value)
        values.append(value)
    after = reference.native_source_inventory(root)
    if source_before != after:
        raise RuntimeError('native source changed')
    reference.write_json(out / 'finish.json', {
        'schema': 'SGLANG_NCU_NATIVE_HOST_V1', 'status': 'PASS_NATIVE_WORKFLOW_AND_ROI',
        'pid': os.getpid(), 'start_ticks': int(Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19]),
        'guarded_parent_pid': parent, 'parent_death_signal': 'SIGKILL',
        'roi': a.roi, 'mode': a.mode, 'profiler_events': boundary.events if a.mode == 'capture' else [],
        'profiler_api_invoked': a.mode == 'capture', 'input_contract': frozen,
        'natural_cuda_event_ms': {phase: events[i][0].elapsed_time(events[i][1])
                                 for i, phase in enumerate(frozen['phases'])} if events else None,
        'event_time_is_NCU_duration': False,
        'natural_phases': completed, 'control_values': values,
        'source_identity': source_before, 'source_unchanged': True,
        'driver_sha256': reference.sha256(__file__),
        'reference_driver_sha256': reference.sha256(reference.__file__),
        'server_args': dataclasses.asdict(args), 'warmup_runs': 1,
        'packages': {name: importlib.metadata.version(name) for name in
                     ('sglang', 'torch', 'sgl-kernel', 'triton', 'flashinfer-python', 'transformers')},
        'model_class': type(runner.model).__module__ + '.' + type(runner.model).__name__,
        'attention_backend_class': type(runner.attn_backend).__module__ + '.' + type(runner.attn_backend).__name__,
        'predictions_fed_back': False, 'numerical_acceptance': 'NOT_ASSESSED',
        'heavy_tensor_registry_hooks': False, 'instruction_memory_trace': False,
        'expected_kernel_count': None,
        'kernel_count_basis': 'NEW_SHAPE_REQUIRES_INDEPENDENT_METADATA_CENSUS',
        'native_cuda_kernels_modified': False,
        'roi_protocol': 'capture: one ProfilerStart/Stop pair; entire frozen workflow runs in every replay; synchronization at each phase boundary; control D2H after measured sequence',
        'wall_clock_is_gpu_duration': False})
    print(json.dumps({'status': 'PASS_NATIVE_WORKFLOW_AND_ROI', 'pid': os.getpid(), 'roi': a.roi}), flush=True)


if __name__ == '__main__':
    main()
