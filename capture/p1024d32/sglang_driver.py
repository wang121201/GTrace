#!/usr/bin/env python3
"""Fixed-input metadata probe for the frozen, installed SGLang 0.4.10.

Uses unmodified bench_one_batch.load_model/extend/decode and native eager
backend kernels. It records tensor descriptors and optional kernel launches,
never activation payloads or instruction-level memory traces.
"""

import argparse
import contextlib
import ctypes
import dataclasses
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import sys
import time
import weakref
import workload


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, default=str) + "\n")


class TensorRegistry:
    def __init__(self, torch):
        self.torch = torch
        self.roots = []
        self.by_storage = {}
        self.tensor_refs = {}
        self.clock = 0

    def describe(self, tensor, label):
        self.clock += 1
        storage = tensor.untyped_storage()
        key = (str(tensor.device), storage._cdata, storage.data_ptr(), storage.nbytes())
        root = self.by_storage.get(key)
        # Do not retain tensors: doing so would change native allocator reuse.
        # A generation is a best-effort Python-visible lifetime, not a claim
        # that backend-private allocations have been completely enumerated.
        if root is not None and not any(ref() is not None for ref in self.tensor_refs[root["id"]]):
            root = None
        if root is None:
            root = {
                "id": "storage_%06d" % len(self.roots),
                "first_label": label,
                "device": str(tensor.device),
                "base_address": storage.data_ptr(),
                "storage_nbytes": storage.nbytes(),
                "storage_identity": storage._cdata,
                "first_observation": self.clock,
                "last_observation": self.clock,
            }
            self.roots.append(root)
            self.by_storage[key] = root
            self.tensor_refs[root["id"]] = []
        root["last_observation"] = self.clock
        refs = self.tensor_refs[root["id"]]
        if not any(ref() is tensor for ref in refs):
            refs[:] = [ref for ref in refs if ref() is not None]
            refs.append(weakref.ref(tensor))
        return {
            "root": root["id"], "label": label,
            "data_address": tensor.data_ptr(),
            "storage_offset_elements": tensor.storage_offset(),
            "storage_offset_bytes": tensor.storage_offset() * tensor.element_size(),
            "shape": list(tensor.shape), "stride_elements": list(tensor.stride()),
            "stride_bytes": [n * tensor.element_size() for n in tensor.stride()],
            "dtype": str(tensor.dtype), "element_size": tensor.element_size(),
            "logical_nbytes": tensor.numel() * tensor.element_size(),
            "device": str(tensor.device),
        }

    def walk(self, value, label):
        if isinstance(value, self.torch.Tensor):
            return [self.describe(value, label)]
        if isinstance(value, dict):
            return [r for k, v in value.items() for r in self.walk(v, label + "." + str(k))]
        if isinstance(value, (tuple, list)):
            return [r for i, v in enumerate(value) for r in self.walk(v, label + "[%d]" % i)]
        return []


class Observer:
    def __init__(self, torch, runner, phases):
        self.torch, self.runner = torch, runner
        self.phases = tuple(phases)
        self.registry = TensorRegistry(torch)
        self.stage = None
        self.events = []
        self.active = []
        self.handles = []
        self.last_forward_batch = None
        self.last_graph_used = None
        self.forward_descriptors = []
        self.native = None
        self.native_epoch = None
        if os.environ.get('SG_NVBIT_SCOPE_ABI') == '1':
            self.native = ctypes.CDLL(None)
            self.native.sg_nvbit_observer_set_scope.argtypes = [ctypes.c_uint64, ctypes.c_int64, ctypes.c_int32,
                                                                ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
            self.native.sg_nvbit_observer_set_scope.restype = ctypes.c_int
            self.native.sg_nvbit_observer_clear_scope.argtypes = []
            self.native.sg_nvbit_observer_clear_scope.restype = ctypes.c_int
            for name in ('sg_nvbit_observer_begin_epoch', 'sg_nvbit_observer_end_epoch'):
                func = getattr(self.native, name)
                func.argtypes = [ctypes.c_uint64]
                func.restype = ctypes.c_int

    def begin_native_epoch(self, number):
        if self.native is not None:
            if self.native.sg_nvbit_observer_begin_epoch(number) != 1:
                raise RuntimeError('Native observer epoch begin failed')
            self.native_epoch = number

    def end_native_epoch(self):
        if self.native is not None and self.native_epoch is not None:
            if self.native.sg_nvbit_observer_end_epoch(self.native_epoch) != 1:
                raise RuntimeError('Native observer epoch end failed')
            self.native_epoch = None

    def update_native_scope(self):
        if self.native is None:
            return
        if self.stage is None:
            if self.native.sg_nvbit_observer_clear_scope() != 1:
                raise RuntimeError('Native observer scope clear failed')
            return
        forward_id = self.phases.index(self.stage)
        layer = -1
        for event, _ in reversed(self.active):
            if event['module_class'].endswith('.LlamaDecoderLayer'):
                layer = int(event['module'].split('.layers.')[1].split('.')[0])
                break
        if self.active:
            event = self.active[-1][0]
            call_id, scope = event['call_id'], event['module']
        else:
            call_id, scope = 10000000 + forward_id, '<phase-global>'
        # A shared module (notably RoPE) keeps its canonical name, while
        # layer ownership comes from the active decoder invocation above.
        rc = self.native.sg_nvbit_observer_set_scope(call_id, forward_id, layer,
                self.stage.encode(), scope.encode(), b'measurement')
        if rc != 1:
            raise RuntimeError('Native launch observer rejected scope marker')

    def install(self):
        for name, module in self.runner.model.named_modules():
            def pre(mod, args, kwargs, name=name):
                if self.stage is None:
                    return
                event = {
                    "call_id": len(self.events), "phase": self.stage,
                    "module": name or "<model>",
                    "module_class": type(mod).__module__ + "." + type(mod).__name__,
                    "parent_call_id": self.active[-1][0]["call_id"] if self.active else None,
                    "inputs": self.registry.walk(args, "args") + self.registry.walk(kwargs, "kwargs"),
                }
                self.events.append(event)
                marker = self.torch.profiler.record_function(
                    "tilegraph/%s/%d/%s" % (self.stage, event["call_id"], name or "<model>")
                )
                marker.__enter__()
                self.active.append((event, marker))
                self.update_native_scope()

            def post(mod, args, kwargs, output):
                if self.stage is None:
                    return
                event, marker = self.active.pop()
                event["outputs"] = self.registry.walk(output, "output")
                marker.__exit__(None, None, None)
                self.update_native_scope()

            self.handles.append(module.register_forward_pre_hook(pre, with_kwargs=True))
            self.handles.append(module.register_forward_hook(post, with_kwargs=True, always_call=True))
        original = self.runner.forward

        def forward(forward_batch, *args, **kwargs):
            if self.stage is not None:
                self.last_forward_batch = forward_batch
                self.forward_descriptors = self.describe_control(forward_batch)
            output = original(forward_batch, *args, **kwargs)
            if self.stage is not None:
                self.last_graph_used = bool(output[1])
                if self.last_graph_used:
                    raise RuntimeError("Eager execution contract violated: CUDA Graph was used")
            return output

        self.runner.forward = forward
        self.original_forward = original

    def describe_control(self, batch):
        fields = ("input_ids", "positions", "seq_lens", "req_pool_indices", "out_cache_loc",
                  "extend_seq_lens", "extend_prefix_lens", "extend_start_loc")
        return [r for key in fields for r in self.registry.walk(getattr(batch, key, None), "forward_batch." + key)]

    def finish(self):
        self.stage = None
        self.update_native_scope()
        self.end_native_epoch()
        self.runner.forward = self.original_forward
        for handle in self.handles:
            handle.remove()


def make_request(bo, frozen):
    ids = list(frozen['prompt_ids'])
    req = bo.Req(rid=0, origin_input_text="", origin_input_ids=ids,
                 sampling_params=bo.SamplingParams(temperature=0, max_new_tokens=frozen['decode_steps'] + 1))
    req.prefix_indices = []
    req.fill_ids = req.origin_input_ids
    req.extend_input_len = len(ids)
    req.logprob_start_len = len(ids) - 1
    return [req]


def control_values(torch, batch):
    fields = ("input_ids", "positions", "seq_lens", "req_pool_indices", "out_cache_loc",
              "extend_seq_lens", "extend_prefix_lens", "extend_start_loc")
    result = {}
    for name in fields:
        value = getattr(batch, name, None)
        if isinstance(value, torch.Tensor):
            if value.numel() > 1024:
                raise RuntimeError("Unexpectedly large control tensor: " + name)
            result[name] = value.detach().cpu().tolist()
        elif value is None or isinstance(value, (int, list, tuple)):
            result[name] = value
    return result


def file_inventory(model_path, hash_weights=False):
    root = Path(model_path).resolve()
    rows = []
    for path in sorted(root.iterdir()):
        if not path.is_file() or path.suffix not in (".json", ".safetensors", ".bin", ".model"):
            continue
        row = {"name": path.name, "path": str(path), "bytes": path.stat().st_size}
        if hash_weights or path.suffix == ".json":
            row["sha256"] = sha256(path)
        rows.append(row)
    return rows


def native_source_inventory(sglang_root):
    paths = ["bench_one_batch.py", "srt/model_executor/model_runner.py",
             "srt/model_executor/forward_batch_info.py", "srt/model_executor/cuda_graph_runner.py",
             "srt/managers/schedule_batch.py", "srt/mem_cache/memory_pool.py",
             "srt/mem_cache/allocator.py", "srt/models/llama.py", "srt/server_args.py",
             "srt/layers/radix_attention.py", "srt/layers/attention/flashinfer_backend.py",
             "srt/layers/attention/triton_backend.py", "srt/layers/linear.py",
             "srt/layers/layernorm.py", "srt/layers/activation.py", "srt/layers/logits_processor.py"]
    return [{"relative": p, "path": str(sglang_root / p), "sha256": sha256(sglang_root / p)} for p in paths]


def compact_profile(torch_profiler, directory, keep_chrome=False):
    raw_path = directory / "torch_launch_metadata.chrome.json"
    torch_profiler.export_chrome_trace(str(raw_path))
    raw = json.loads(raw_path.read_text())
    events = raw.get("traceEvents", [])
    # Keep launch correlations, nested record_function markers and GPU events.
    # No per-address memory access events or tensor payloads are requested.
    selected = [e for e in events if e.get("cat") in (
        "kernel", "gpu_memcpy", "gpu_memset", "cuda_runtime", "cuda_driver", "user_annotation")
        or e.get("ph") in ("s", "f")]
    write_json(directory / "kernel_launches.json", {
        "source": "torch.profiler CPU/CUDA metadata", "profile_memory": False,
        "memory_instruction_trace": False, "events": selected,
        "kernel_count": sum(e.get("cat") == "kernel" for e in selected),
        "note": "Associate launch correlation and enclosing tilegraph markers; backend-private tensors remain unbound until separately observed.",
    })
    if not keep_chrome:
        raw_path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-path", default="/home/xmu/.cache/modelscope/hub/models/LLM-Research/Meta-Llama-3-8B-Instruct")
    parser.add_argument("--output-dir", "--output", dest="output_dir", required=True)
    parser.add_argument("--attention-backend", choices=("flashinfer", "triton"), default="flashinfer")
    parser.add_argument("--mem-fraction-static", type=float, choices=(0.90,), default=0.90)
    workload.add_arguments(parser)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--keep-chrome-profile", action="store_true")
    parser.add_argument("--hash-weights", action="store_true")
    args = parser.parse_args()
    frozen = workload.from_args(args)
    if args.warmup_runs != 1 or args.attention_backend != 'flashinfer':
        parser.error("Frozen native contract requires one warmup and flashinfer")
    out = Path(args.output_dir).resolve()
    out.mkdir(parents=True, exist_ok=False)

    # Imports occur only on an explicit invocation, so --help and py_compile
    # are available on the local CPU host without installing SGLang.
    import torch
    import sglang
    import sglang.bench_one_batch as bo
    workload.check_packages()

    source_before = native_source_inventory(Path(sglang.__file__).resolve().parent)
    workload.check_native_sources(source_before)
    native_args = bo.ServerArgs(
        model_path=args.model_path, dtype="bfloat16", load_format="safetensors",
        device="cuda", tp_size=1, pp_size=1, attention_backend=args.attention_backend,
        disable_cuda_graph=True, cuda_graph_max_bs=1, enable_torch_compile=False,
        disable_overlap_schedule=True, disable_radix_cache=True,
        mem_fraction_static=args.mem_fraction_static, max_total_tokens=args.max_total_tokens,
        max_running_requests=1, random_seed=0, cpu_offload_gb=0,
    )
    bo._set_envs_and_config(native_args)
    runner, _tokenizer = bo.load_model(native_args, bo.PortArgs.init_new(native_args), 0)
    if runner.cuda_graph_runner is not None:
        raise RuntimeError("Eager contract requires no CUDA Graph runner")
    if type(runner.model).__name__ != "LlamaForCausalLM":
        raise RuntimeError("Unexpected native model implementation: " + type(runner.model).__name__)
    if runner.model_config.hf_config.num_hidden_layers != 32:
        raise RuntimeError("The workload contract requires all 32 original model layers")
    device = runner.device
    fixed_decode = [torch.tensor([token], dtype=torch.int64, device=device) for token in frozen['decode_input_ids']]
    with torch.no_grad():
        for _ in range(args.warmup_runs):
            runner.req_to_token_pool.clear()
            runner.token_to_kv_pool_allocator.clear()
            _, _, batch = bo.extend(make_request(bo, frozen), runner)
            for token in fixed_decode:
                bo.decode(token, batch, runner)
        torch.cuda.synchronize()
    runner.req_to_token_pool.clear()
    runner.token_to_kv_pool_allocator.clear()

    observer = Observer(torch, runner, frozen['phases'])
    observer.install()
    parameters = [observer.registry.describe(t, "parameter." + name)
                  for name, t in runner.model.named_parameters(remove_duplicate=False)]
    buffers = [observer.registry.describe(t, "buffer." + name)
               for name, t in runner.model.named_buffers(remove_duplicate=False)]
    kv = runner.token_to_kv_pool
    kv_buffers = [observer.registry.describe(t, "kv.%s.%d" % (kind, layer))
                  for kind in ("k_buffer", "v_buffer")
                  for layer, t in enumerate(getattr(kv, kind))]
    page_table = observer.registry.describe(runner.req_to_token_pool.req_to_token, "req_to_token")
    profiler = torch.profiler.profile(
        activities=[torch.profiler.ProfilerActivity.CPU, torch.profiler.ProfilerActivity.CUDA],
        record_shapes=False, profile_memory=False, with_stack=False,
    ) if args.profile else contextlib.nullcontext()
    stages = []
    try:
        with torch.no_grad(), profiler as active_profiler:
            batch = None
            for index, phase in enumerate(frozen['phases']):
                torch.cuda.synchronize()
                observer.begin_native_epoch(index + 1)
                observer.stage = phase
                observer.update_native_scope()
                begin = time.perf_counter()
                with torch.profiler.record_function("phase/" + phase):
                    torch.cuda.nvtx.range_push("phase/" + phase)
                    try:
                        if index == 0:
                            predicted, logits, batch = bo.extend(make_request(bo, frozen), runner)
                        else:
                            predicted, logits = bo.decode(fixed_decode[index - 1], batch, runner)
                    finally:
                        torch.cuda.nvtx.range_pop()
                torch.cuda.synchronize()
                elapsed = time.perf_counter() - begin
                observer.stage = None
                observer.update_native_scope()
                observer.end_native_epoch()
                actual = control_values(torch, observer.last_forward_batch)
                expected_inputs = workload.expected_controls(frozen, index)['input_ids']
                if actual["input_ids"] != expected_inputs or actual["seq_lens"] != [frozen['prefill_length'] + index]:
                    raise RuntimeError("Native batch did not preserve frozen inputs: " + repr(actual))
                workload.validate_controls(frozen, index, dict(input_ids=actual['input_ids'],
                    positions=actual['positions'], seq_lens_sum=actual['seq_lens'][0], out_cache_loc=actual['out_cache_loc']))
                row = int(actual["req_pool_indices"][0])
                slots = runner.req_to_token_pool.req_to_token[row, :frozen['prefill_length'] + index].detach().cpu().tolist()
                phase_data = {
                    "phase": phase, "forward_index": index, "batch_size": 1,
                    "fixed_input_ids": expected_inputs, "actual_forward_batch": actual,
                    "forward_tensor_descriptors": observer.forward_descriptors,
                    "kv_token_slots": slots, "kv_page_size": kv.page_size,
                    "cuda_graph_used": observer.last_graph_used,
                    "predicted_token_ids": predicted.detach().cpu().tolist(),
                    "predictions_fed_back": False,
                    "logits": observer.registry.describe(logits, phase + ".logits"),
                    "instrumented_elapsed_seconds": elapsed,
                    "numerical_acceptance": "NOT_ASSESSED",
                }
                stages.append(phase_data)
                write_json(out / (phase + ".json"), phase_data)
                print("%s complete, input length %d, prefix %d, instrumented %.2f s" %
                      (phase, len(expected_inputs), frozen['prefill_length'] + index, elapsed), flush=True)
        if args.profile:
            compact_profile(active_profiler, out, args.keep_chrome_profile)
    finally:
        observer.stage = None
        observer.finish()

    source_after = native_source_inventory(Path(sglang.__file__).resolve().parent)
    if source_before != source_after:
        raise RuntimeError("Native source identity changed during execution")
    packages = {}
    for name in ("sglang", "torch", "sgl-kernel", "triton", "flashinfer-python", "transformers"):
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            packages[name] = None
    manifest = {
        "schema": "sglang-fixed-input-metadata-v1", "status": "COMPLETE",
        "driver_sha256": sha256(__file__), "python": sys.version, "platform": platform.platform(),
        "process": {"pid": os.getpid(), "start_ticks": int(Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19])},
        "packages": packages, "torch_cuda": torch.version.cuda,
        "gpu": {"name": torch.cuda.get_device_name(), "capability": list(torch.cuda.get_device_capability())},
        "native_source_files": source_before, "native_source_unchanged": True,
        "server_args": dataclasses.asdict(native_args),
        "resolved_attention_backend_class": type(runner.attn_backend).__module__ + "." + type(runner.attn_backend).__name__,
        "model_class": type(runner.model).__module__ + "." + type(runner.model).__name__,
        "hf_config": runner.model_config.hf_config.to_dict(),
        "model_files": file_inventory(args.model_path, args.hash_weights),
        "weight_content_hashes_complete": args.hash_weights,
        "input_contract": frozen,
        "execution_contract": {"native_eager": True, "native_sampling_retained": True, "cuda_graph": False,
                               "full_model_layers": 32, "warmup_runs": args.warmup_runs,
                               "kernel_implementation_modified": False, "numerical_acceptance": "NOT_ASSESSED"},
        "coverage": {"parameter_and_module_boundary_metadata": True, "backend_private_tensors_complete": False,
                     "native_scope_abi_enabled": observer.native is not None,
                     "kernel_launch_metadata": args.profile, "tilegraph_complete": False,
                     "instruction_memory_trace_collected": False,
                     "lifetime_tracking": "Python tensor weakrefs; backend-private allocations may be unobserved",
                     "timing": "instrumented diagnostic only; module hooks and stage control D2H add overhead"},
        "kv_pool": {"class": type(kv).__module__ + "." + type(kv).__name__, "page_size": kv.page_size,
                    "size": kv.size, "dtype": str(kv.dtype), "buffers": kv_buffers, "page_table": page_table},
        "parameters": parameters, "module_buffers": buffers, "stage_files": [s["phase"] + ".json" for s in stages],
    }
    write_json(out / "tensor_roots.json", observer.registry.roots)
    write_json(out / "module_calls.json", observer.events)
    write_json(out / "manifest.json", manifest)
    files = [{"path": str(p.relative_to(out)), "bytes": p.stat().st_size, "sha256": sha256(p)}
             for p in sorted(out.iterdir()) if p.is_file()]
    write_json(out / "files.sha256.json", files)
    print("Metadata collection complete: " + str(out), flush=True)


if __name__ == "__main__":
    main()
