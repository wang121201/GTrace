"""Pinned, opt-in calibration profiles; r3 was not promoted by its authors."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = 'r2-adaptive'
TRACE_SHA256 = 'c4e4d8e85e9049af5694afca10b031fae4eafcdd06f0dbd3d8042d7195db890b'
PROFILES = {
 'tuner-v1': ('v1', 'NVIDIA_RTX_4000_Ada_Generation', 'e09511b9d168632e975422b19a9557ff5ed4482ace160fa428f9c70455f5a891'),
 'r2-adaptive': ('r2', '01_measured_adaptive', 'dabaf5334ea681c419e5ea8d820ead7c03246dea059b0ca812d163c7968ac79c'),
 'r2-shared64': ('r2', '02_shared64', '3c11c042edc312548d2ca1732d9c196a779826dd212b2f5c9cf1a413c4baf65a'),
 'r2-shared100': ('r2', '03_shared100', '36c280a2f7c43f7b8ea2a4b8a15988760fdc5244f32b8a4893727f57f24abaae'),
 'r3-fifo-shared32': ('r3', '07_fifo_static_shared32', '7f7f970a6e6acbfea15aa2e652d1133048be716d215fcf0e31385dffb25adc4c'),
 'r3-fifo-shared64': ('r3', '08_fifo_static_shared64', 'd47ce630e89bffaf4b6174007db5f466c2b62cc431d75204947b3a04974c45f3'),
 'r3-fifo-shared100': ('r3', '09_fifo_static_shared100', '70e9f6f4b541a77bfd803ba97f98bae10c3fa9aa46daa01b718a26c407061264'),
}
def profile_dir(name):
 if name not in PROFILES:
  raise ValueError('unknown Ada profile: '+name)
 return ROOT/'configs'/('rtx4000-ada-accelsim-v1' if name=='tuner-v1' else 'rtx4000-ada-calibrated/'+name)
