# GTSim v0.1.0 release scope

GTSim v0.1.0 is the first public core release. It provides the graph-driven
simulation backend and exactly two hand-written H100 reference tile-graph
builders: persistent two-stage GEMM and FA3.

The following paper-described capabilities are not included in v0.1.0:

- a general TileLang-IR frontend or automatic graph construction;
- CTA graph-template sharing;
- workload builders beyond the supplied H100 GEMM and FA3 references;
- B200/FA4 case-study code and its validation path; and
- the full set of runtime engineering and validation experiments used in the
  paper.

v0.1.0 includes the simulator core, H100 GEMM, and H100 FA3. It does not
include the other components described in the paper.
