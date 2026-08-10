/* Driver for js_module_double_result_smoke — kept out of the .shcc so the
 * node source stays under the AST text-field limit. */
const c = require(process.cwd() + '/bin/gamut_counter.node');
console.log('scale', c.scale(0.5));
console.log('label', c.label());
const b = c.big();
console.log('big', typeof b, b.toString());
console.log('bigint_arg', c.bump(1n));
const uhi = (1n << 63n) + 5n;
console.log('u64_hi', c.echo_u64(uhi).toString());
console.log('u64_max', c.echo_u64((1n << 64n) - 1n).toString());
try { c.echo_u64(-1n); } catch (e) { console.log('u64_neg:', e.constructor.name); }
try { c.bump(1, 2); } catch (e) { console.log('arity:', e.message); }
try { c.bump({by: 1, wrong: 1}); } catch (e) { console.log('unexpected:', e.message); }
try { c.bump('x'); } catch (e) { console.log('badtype:', e.message); }
console.log('half', c.half(5));
try { c.half(-1); } catch (e) { console.log('half_raised:', e.constructor.name, e.code + ':', e.message); }
console.log('sum_array', c.sum([1, 2, 3.5]));
console.log('sum_typed', c.sum(new Float64Array([1, 2, 3.5])));
console.log('sum_f32', c.sum(new Float32Array([1, 2, 3.5])));
const buf = new Float64Array(3);
c.fill(buf, 9);
console.log('filled', buf[0], buf[1], buf[2]);
const row = c.row();
console.log('row', row.constructor.name, Array.from(row).join(','));
console.log('roundtrip', c.sum(c.row()));
try { c.sum(['x']); } catch (e) { console.log('badelem:', e.message); }
console.log('apply', c.apply({step: (x) => x * 2}));
console.log('kw_bag', c.bump({by: 2}));
console.log('pos_plain', c.peek_opts({x: 7}));
console.log('pos_kw', c.peek_opts({opts: {x: 7}, n: 3}));
console.log('pos_escape', c.peek_opts({__cc_js_pos__: {x: 7, opts: 99}}));
console.log('eval42', c.eval42());
console.log('eval_chain', c.eval_chain());
console.log('eval_i64', c.eval_typed());
console.log('gparse', c.gparse());
console.log('chain_floor', c.chain_floor());
console.log('chain_pi', c.chain_pi());
console.log('chain_pid_ok', c.chain_pid() === process.pid);
console.log('rowmap', c.rowmap((x, k) => x * k + 0.25,
                               new Float64Array([1.5, 2.5, 3.5]),
                               new BigInt64Array([2n, 3n, 4n])));
