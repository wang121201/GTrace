"""Pure file/hash/domain checks; never imports a model or invokes a compiler."""
from pathlib import Path
import hashlib,json
D=Path(__file__).resolve().parent

def pin(p):
    p=Path(p);b=p.read_bytes()
    return {'path':str(p),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()}

def main():
    proof=json.loads((D/'source-proof.json').read_text());checks=0
    def need(v):
        nonlocal checks
        assert v
        checks+=1
    pins=proof['source_pins']+[x['dependency_file'] for x in proof['dependent_TUs']+proof['independent_HBFSIM_TUs']]
    for p in pins:need(pin(p['path'])==p)
    old=Path(proof['original']['path']).read_text();new=(D/'writer_observer.h').read_text()
    need(pin(D/'writer_observer.h')==proof['candidate'])
    need(old.count(proof['old_literal'])==1 and new.count(proof['new_literal'])==1)
    need(new.replace(proof['new_literal'],proof['old_literal']).encode()==Path(proof['original']['path']).read_bytes())
    overlay=json.loads((D/'overlay.json').read_text())
    need(overlay=={'version':0,'case-sensitive':True,'use-external-names':False,'roots':[{'type':'file','name':proof['original']['path'],'external-contents':str(D/'writer_observer.h')}]})
    header=(D/'history_attribution.h').read_text()
    need('#include "'+proof['original']['path']+'"' in header)
    need('observation_(0,count_-1,false,nullptr,3020)' in header)
    # Domain algebra only, not C++ execution: old defaults and the explicit new bound.
    def gate(first,last,bound=1138):return bound>0 and first>=0 and last>=first and last-first<bound
    cases=[((0,1137),True),((0,1138),False),((408,1545),True),((408,1546),False),((-1,1),False),((1,0),False),((0,3017,3020),True),((0,3019,3020),True),((0,3020,3020),False),((0,0,0),False)]
    for args,expected in cases:need(gate(*args)==expected)
    for r in proof['dependent_TUs']:need('writer_observer.h' in Path(r['dependency_file']['path']).read_text())
    for r in proof['independent_HBFSIM_TUs']:need('writer_observer.h' not in Path(r['dependency_file']['path']).read_text())
    need([r['historical_step'] for r in proof['dependent_TUs']]==['00','01','02','03','04','05','06'])
    return {'schema':'CURRENT_HISTORY_WRITER_SOURCE_CHECKS_V1','status':'PASS_PURE_SOURCE_CHECKS','checks':checks,'negative_domain_cases':sum(not x[1] for x in cases),'compiler_invoked':False,'simulation_executed':False,'GPU_executed':False,'source_pins_checked':len(pins),'actual_model_fields_regression_performed':False}

if __name__=='__main__':print(json.dumps(main(),indent=2))
