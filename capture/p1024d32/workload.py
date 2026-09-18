"""Frozen-input contract shared by metadata and NCU hosts; no GPU imports."""
import hashlib
import json
from pathlib import Path

MODEL = '/home/xmu/.cache/modelscope/hub/models/LLM-Research/Meta-Llama-3-8B-Instruct'
PACKAGES = {'sglang': '0.4.10', 'torch': '2.7.1+cu126', 'sgl-kernel': '0.2.8',
            'triton': '3.3.1', 'flashinfer-python': '0.2.9rc2', 'transformers': '4.54.1'}
MODEL_SHA = {
    'config.json': '61f3de03a16ca8046b05dc777bce72717022bc8522152eee61a69072272ef54b',
    'model.safetensors.index.json': '146776fce3f6db1103aa6f249e65ee5544c5923ce6f971b092eee79aa6e5d37b',
    'model-00001-of-00004.safetensors': 'd8cf9c4d0dd972e1a2131bfe656235ee98221679711a3beef6d46dadf0f20b5c',
    'model-00002-of-00004.safetensors': '8d4782b4a69ef03845159ce1a15e272aadaaf134dc138d68f616098e8531729c',
    'model-00003-of-00004.safetensors': '3acdd690e65c24f42a24581b8467af98bd3ca357444580f8012aacd2bd607921',
    'model-00004-of-00004.safetensors': '67e9ad31c8c32abf3a55ee7fc7217b3ecb35fd3c74d98a5bd233e0e4d6964f46',
}


def add_arguments(parser):
    parser.add_argument('--prefill-length', type=int, default=1024)
    parser.add_argument('--decode-steps', type=int, default=32)
    parser.add_argument('--max-total-tokens', type=int, default=1280)


def contract(prefill_length=1024, decode_steps=32, max_total_tokens=1280):
    if not (1 <= prefill_length <= 1024 and 1 <= decode_steps <= 32):
        raise ValueError('bounded workload requires 1..1024 prefill and 1..32 decode')
    if not prefill_length + decode_steps + 1 <= max_total_tokens <= 4096:
        raise ValueError('capacity must include P+D plus one allocator slot, and be <=4096')
    value = dict(schema='SGLANG_FIXED_PD_INPUT_V1', batch_size=1, prefill_length=prefill_length,
                 decode_steps=decode_steps, prompt_ids=list(range(1000, 1000 + prefill_length)),
                 decode_input_ids=[(944, 291)[i % 2] for i in range(decode_steps)],
                 phases=['Prefill'] + ['Decode%d' % i for i in range(1, decode_steps + 1)],
                 max_total_tokens=max_total_tokens, output_feedback=False,
                 input_source='Frozen arithmetic prompt IDs; alternating 944/291 decode IDs; no tokenizer/chat template',
                 model=MODEL, dtype='bfloat16', layers=32, tp_size=1, pp_size=1,
                 attention_backend='flashinfer', native_eager=True, warmup_runs=1,
                 mem_fraction_static=0.90, cuda_graph=False, torch_compile=False,
                 disable_overlap_schedule=True, disable_radix_cache=True,
                 sampling_retained=True, numerical_acceptance='NOT_ASSESSED')
    value['sha256'] = hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return value


def from_args(args):
    return contract(args.prefill_length, args.decode_steps, args.max_total_tokens)


def expected_controls(c, index):
    if not 0 <= index <= c['decode_steps']:
        raise ValueError('phase index outside workload')
    p = c['prefill_length']
    return dict(input_ids=c['prompt_ids'] if index == 0 else [c['decode_input_ids'][index - 1]],
                positions=list(range(p)) if index == 0 else [p + index - 1],
                seq_lens_sum=p + index,
                out_cache_loc=list(range(1, p + 1)) if index == 0 else [p + index])


def validate_controls(c, index, values):
    for name, expected in expected_controls(c, index).items():
        if values.get(name) != expected:
            raise ValueError('actual native %s differs at %s' % (name, c['phases'][index]))


def check_packages():
    import importlib.metadata
    actual = {name: importlib.metadata.version(name) for name in PACKAGES}
    if actual != PACKAGES:
        raise RuntimeError('native package identity changed: ' + repr(actual))
    return actual


def check_native_sources(rows):
    reference = json.loads((Path(__file__).resolve().parent / 'native-source-reference.json').read_text())
    actual = {r['relative']: r['sha256'] for r in rows}
    expected = {r['relative']: r['sha256'] for r in reference['files']}
    if len(actual) != len(rows) or actual != expected:
        raise RuntimeError('native SGLang Python sources differ from frozen baseline')
