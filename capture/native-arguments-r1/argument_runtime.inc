// Included inside sgobs after State/Function. Host buffers only, before launch.
string capture_native_arguments(uint64_t native_id,const Function& d,uint64_t cid,uint64_t stream,const string& api,
 unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,unsigned dynamic_shared,
 int static_shared,int registers,int local_bytes,const string& attrs,void** params,void** extra) {
  State& s=state();sgargs::Actual a;
  a.epoch=s.active_epoch;a.forward=scope.forward;a.layer=scope.layer;a.phase=scope.phase;a.module=scope.module;
  a.api=api;a.code=d.code_hash;a.code_kind="sha256_nvbit_decoded_instruction_rows_v1";a.layout=sha(uarray(d.args));a.sizes=d.args;a.attrs=attrs;
  a.grid={{gx,gy,gz}};a.block={{bx,by,bz}};a.dynamic_shared=dynamic_shared;
  a.static_shared=uint64_t(static_shared);a.registers=uint64_t(registers);a.local_bytes=uint64_t(local_bytes);
  a.pid=s.owner;a.start_ticks=s.ticks;a.native_launch_id=native_id;a.call_id=scope.call;a.context_id=cid;a.function_id=d.id;a.stream=stream;
  a.scope_bound=scope.bound;a.role=scope.role;
  auto record=s.argument_ledger.before(a,params,extra);
  if(!record.selected)return string();
  const string payload=sgargs::serialize(record,sha),row=payload+"\n";
  sgargs::check_output_budget(s.argument_ledger.limits(),s.argument_file.bytes,row.size());
  s.emit(s.argument_file,row);
  return ",\"native_argument_record\":{\"file\":\"launch-arguments.jsonl\",\"sequence\":"+number(record.sequence)+
    ",\"payload_sha256\":"+quote(sha(payload))+"}";
}
