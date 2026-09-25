"""Copy the frozen runtime into this experiment; only select an explicit profile."""
from pathlib import Path
import json
from prepare import D, H, pin, write_new


def main():
    out = D / 'runtime-r2'
    out.mkdir(exist_ok=False)
    source = H / 'history_runtime-r3.cpp'
    text = source.read_text()
    edits = []
    # Resolve includes against the original source, without copying or altering it.
    for line in text.splitlines():
        if line.startswith('#include "'):
            name = line.split('"')[1]
            if not name.startswith('/'):
                target = (H / name).resolve()
                assert target.is_file(), target
                edits.append((line, '#include "' + str(target) + '"'))
    edits += [
        ('namespace history_case {',
         '#include "' + str(D / 'structure-adapter/config.h') + '"\nnamespace history_case {'),
        ('static g::SimulatorConfig config(){auto c=g::make_rtx4000_ada_footprint_reference_config();',
         'static g::SimulatorConfig config(const J&p){auto c=g::make_ada_cosim_alignment_config(p.at("ada_alignment_profile").get<std::string>());'),
        ('Device(const J&p,const coupling::ServiceMapper&map):cfg(config()),backend(p),',
         'Device(const J&plan,const coupling::ServiceMapper&map):cfg(config(plan)),backend(plan.at("profile")),'),
        ('need(std::abs(1e6*p.at("clock").at("period_ps_denominator").get<double>()/p.at("clock").at("period_ps_numerator").get<double>()-cfg.core_frequency_mhz)',
         'need(std::abs(1e6*plan.at("profile").at("clock").at("period_ps_denominator").get<double>()/plan.at("profile").at("clock").at("period_ps_numerator").get<double>()-cfg.core_frequency_mhz)'),
        ('auto cfg=Device::config();', 'auto cfg=Device::config(plan);'),
        ('Device d(plan.at("profile"),map);', 'Device d(plan,map);'),
        ('Mapper base;auto cfg=Device::config();',
         'if(plan.value("full_measured",false)){\n'
         ' need(!plan.at("full_history").get<bool>()&&!plan.at("complete_prefix_from_process_start").get<bool>()&&plan.at("initial_cache_state")=="EMPTY_EXPLICIT_DIAGNOSTIC","explicit cold measured entry");\n'
         ' need(plan.at("counts")==J({{"native_kernel",1138},{"memory_api_submission",29},{"epoch_begin",3},{"epoch_end",3}})&&plan.at("timeline").size()==1173,"complete measured event census");\n'
         ' need(plan.at("kernel_work").at("CTAs")==772512&&plan.at("kernel_work").at("nodes")==4712768255ULL,"complete measured source work");\n'
         ' U id=1288;std::map<std::string,U> phases;for(const auto&e:plan.at("timeline")){need(e.at("epoch").get<int>()>=4&&e.at("epoch").get<int>()<=6,"only measured epochs");if(e.at("kind")=="native_kernel"){need(e.at("native_launch_id")==id++,"complete consecutive native measured calls");++phases[e.at("phase").get<std::string>()];}}\n'
         ' need(id==2426&&J(phases)==J({{"Measured/Prefill",408},{"Measured/Decode1",365},{"Measured/Decode2",365}}),"all three measured phases");\n'
         ' }\n Mapper base;auto cfg=Device::config();'),
        ('std::ofstream(output)<<result.dump(2)',
         'result["ada_alignment"]={{"profile",plan.at("ada_alignment_profile")},{"core_MHz",d.cfg.core_frequency_mhz},{"L1_hit_cycles",d.cfg.per_sm_l1.hit_latency_cycles},{"L2_hit_cycles",d.cfg.l2_hit_latency_cycles},{"L2_bytes",d.cfg.l2_cache_size_bytes},{"L1_capacity_bytes_per_SM",d.cfg.per_sm_l1.capacity_bytes_per_sm},{"external_HBFSIM_route_aligned_to_AccelSim",false},{"L1_r4_adopted",false},{"whole_line_RFO_preserved",true},{"internal_604_parameter_executed",false},{"hardware_accuracy_claimed",false}};\n'
         ' result["initial_cache_state"]=plan.value("initial_cache_state",std::string("PROCESS_START_EMPTY"));\n'
         ' result["full_measured_complete"]=plan.value("full_measured",false);\n'
         ' result["full_inference"]=plan.value("full_measured",false)||plan.at("full_history").get<bool>();\n'
         ' std::ofstream(output)<<result.dump(2)'),
    ]
    # Apply longer substitutions first where a changed statement contains a shorter one.
    edits.sort(key=lambda pair: len(pair[0]), reverse=True)
    for before, after in edits:
        assert text.count(before) == 1, before
        text = text.replace(before, after)
    destination = out / 'history_runtime.cpp'
    with destination.open('x') as f:
        f.write(text)
    write_new(out / 'derivation.json', dict(status='DERIVED_NOT_COMPILED', original=pin(source),
        generated=pin(destination), generator=pin(__file__),
        edits=[dict(before=b, after=a) for b, a in edits],
        unchanged=['source workload', 'Tiny/Fine dispatchers', 'API q1',
                   'q16/GEMV phase16/kernel epoch8', 'shared h288 policy',
                   'HBFSIM generic GDDR6 config and route', '32B writeback', 'no end flush']))
    print(json.dumps(dict(status='DERIVED_NOT_COMPILED', generated=pin(destination))))


if __name__ == '__main__':
    main()
