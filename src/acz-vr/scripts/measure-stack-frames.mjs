#!/usr/bin/env node
// Offline x64 MSVC /FAs or dumpbin /DISASM stack screening; no dependencies.
// Budget: known deepest chain must be <16 KiB; warn for every frame >1 KiB.
// 16 KiB is 1/64 of a 1 MiB game-thread stack, leaving margin for the game,
// but the game's existing depth and external/indirect callees remain unknown.
// Sum static pushes (8 each), sub rsp allocations and an 8-byte return address.
// Summing mutually exclusive allocations is conservative, not CFG analysis.
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const BUDGET = 16 * 1024;
const WARN = 1024;
const ROOTS = ['writeBindingSrv', 'copyBindingDescriptors', 'copyBindingSimple',
  'hookedDrawIndexedInstanced', 'hookedDrawInstanced'];
const LIMITS = [
  'Inlining: inlined functions are already included in the caller frame; only listing symbols are measured.',
  'Coverage: missing/indirect/external calls, tail jumps and dynamic RSP writes are unresolved; no full stack bound or runtime parity is proved.',
  'Accounting: static pushes + static sub rsp + 8-byte return address; summed allocations may overestimate mutually exclusive paths.',
  'Recursion has no finite bound without a depth limit. Shadow space belongs to caller allocation; no extra 32 bytes added per edge.'
];

function immediate(value) {
  const s = value.trim();
  if (/^0x[\da-f]+$/i.test(s)) return Number.parseInt(s.slice(2), 16);
  if (/^[\da-f]+h$/i.test(s)) return Number.parseInt(s.slice(0, -1), 16);
  if (/^\d+$/.test(s)) return Number(s);
  return null;
}

export function parseListing(text) {
  const functions = new Map();
  const addresses = new Map();
  const variants = [];
  const addressSeen = new Set();
  let current = null;
  let inProc = false;
  let accumulator = null;
  // Retain local labels and instruction effects before merging COMDAT variants.
  const flows = new Map();
  const localKey = value => {
    const target = value.replace(/^(?:SHORT|NEAR(?: PTR)?)\s+/i, '').replace(/^<(.+)>$/, '$1').trim();
    return /^[\da-f]{8,16}$/i.test(target) ? BigInt(`0x${target}`).toString() : target;
  };
  // Numeric branch destinations have no separate label line. Clear constants
  // there as well, so fallthrough facts cannot masquerade as facts at a join.
  const numericJoins = new Set([...text.matchAll(/\b(?:j\w+|loop\w*)\s+(?:(?:SHORT|NEAR(?: PTR)?)\s+)?([\da-f]{8,16})\b/gi)]
    .map(m => localKey(m[1])));
  const begin = name => {
    current = {name, bytes: 8, pushes: 0, allocation: 0, calls: [], notes: [], unknown: [], instructions: 0};
    variants.push(current);
    flows.set(current, {labels: new Map(), steps: []});
    accumulator = null;
  };
  for (const raw of text.split(/\r?\n/)) {
    // /FAs names are decorated, with an optional demangled comment after PROC.
    let m = raw.match(/^\s*(\S+)\s+PROC\b/i);
    if (m) { begin(m[1]); inProc = true; continue; }
    if (/^\s*\S+\s+ENDP\b/i.test(raw)) { current = null; inProc = false; continue; }
    // /FAs local labels must not become new function symbols.
    m = raw.split(';')[0].trim().match(/^(\S+):$/);
    if (m && current && (inProc || /^[.$]/.test(m[1]))) {
      flows.get(current).labels.set(localKey(m[1]), flows.get(current).steps.length);
      accumulator = null; // another predecessor may have changed RAX
      continue;
    }
    // dumpbin symbol labels; instruction addresses are followed by bytes, not EOL.
    m = raw.match(/^\s*([^\s;].*):\s*$/);
    if (m && !/^[\da-f]+$/i.test(m[1]) && !/^[.$]/.test(m[1])) { begin(m[1]); continue; }
    if (!current) continue;
    const address = raw.match(/^\s*([\da-f]{8,16}):/i)?.[1];
    if (address && !addressSeen.has(current)) {
      addresses.set(BigInt(`0x${address}`).toString(), current.name);
      addressSeen.add(current);
    }
    if (address) {
      flows.get(current).labels.set(localKey(address), flows.get(current).steps.length);
      if (numericJoins.has(localKey(address))) accumulator = null;
    }
    let line = raw.split(';')[0].trim();
    line = line.replace(/^[\da-f]+:\s*/i, '').replace(/^[\da-f]{4,}\s+(?=[\da-f]{2}\s)/i, '');
    line = line.replace(/^(?:[\da-f]{2}\s+)+/i, '').trim();
    m = line.match(/^(\w+)\s*(.*)$/);
    if (!m) continue;
    const op = m[1].toLowerCase(), arg = m[2].trim();
    if (/^(?:push|pushfq|pop|sub|add|mov|lea|call|jmp|ret|xor|test|cmp|nop|int|v?mov\w*)$/.test(op)) current.instructions++;
    const flow = flows.get(current);
    const branch = /^(?:j\w+|loop\w*)$/.test(op);
    const step = {delta: 0, growth: false, unknown: false, branch, call: op === 'call', target: localKey(arg)};
    flow.steps.push(step);
    if (op === 'push' || op === 'pushfq') {
      current.pushes++; current.bytes += 8;
      step.delta = 8; step.growth = true;
    }
    if (op === 'pop' || op === 'popfq') step.delta = -8;
    // Narrow proof only: immediate mov eax/rax,N; optional call __chkstk;
    // then sub/add rsp,rax. Every other instruction kills the cache, including
    // all alias mentions and implicit writes. Do not propagate register copies.
    const before = accumulator;
    accumulator = null;
    if (op === 'mov' && /^(?:e|r)ax\s*,/i.test(arg)) accumulator = immediate(arg.split(',')[1]);
    if ((op === 'sub' || op === 'add') && /^rsp\s*,/i.test(arg)) {
      const rhs = arg.split(',')[1].trim();
      const n = /^rax$/i.test(rhs) ? before : immediate(rhs);
      if (n === null) {
        current.unknown.push(op === 'sub' ? `dynamic allocation: ${line}` : `nonconstant RSP adjustment: ${line}`);
        step.unknown = true; step.growth = true;
      } else {
        step.delta = op === 'sub' ? n : -n;
        step.growth = step.delta > 0;
        if (op === 'sub') { current.allocation += n; current.bytes += n; }
      }
      accumulator = null;
    } else if ((/^rsp\s*,/i.test(arg) && !['cmp', 'test'].includes(op)) ||
               (op === 'pop' && /^rsp$/i.test(arg))) {
      current.unknown.push(`RSP write / lea / alloca pattern: ${line}`);
      step.unknown = true; step.growth = true;
    }
    // Unmodelled stack operations cannot justify the net-zero exception.
    if (/^(?:enter|leave|ret|iret\w*)$/.test(op) ||
        (/\b(?:rsp|esp|sp|spl)\b/i.test(arg) &&
         !((op === 'sub' || op === 'add') && /^rsp\s*,/i.test(arg)))) step.unknown = true;
    if (branch) accumulator = null;
    if (op === 'call' || op === 'jmp') {
      if (op === 'call' && /^(?:__chkstk|<__chkstk>)$/.test(arg)) {
        current.notes.push('__chkstk probe (not an extra frame allocation)');
        if (op === 'call') { accumulator = before; step.call = false; }
        continue; // MSVC __chkstk preserves RAX for subsequent sub rsp,rax.
      }
      // Local branches do not add frames. Nonlocal tail jumps need inspection.
      if (op === 'jmp') { step.jump = true; continue; }
      const indirect = /\[|\bPTR\b|^(?:r(?:ax|bx|cx|dx|si|di|sp|bp|\d+)|[xyz]mm\d+)$/i.test(arg);
      current.calls.push({target: arg.replace(/^<(.+)>$/, '$1'), indirect, tail: op === 'jmp'});
      accumulator = null;
    }
  }
  for (const f of variants) {
    const {labels, steps} = flows.get(f);
    for (const [index, step] of steps.entries()) {
      if (!step.branch) continue;
      const target = labels.get(step.target);
      if (step.jump && target === undefined) f.calls.push({target: step.target, indirect: /\[|\bPTR\b|^r\w+$/i.test(step.target), tail: true});
      if (target === undefined) {
        // Conditional targets outside the decoded procedure cannot prove a bound.
        if (!step.jump) f.unknown.push(`unresolved local branch: ${step.target}`);
        continue;
      }
      if (target > index) continue; // forward branch, not a cycle
      const region = steps.slice(target, index + 1);
      // A straight-line cycle is balanced only if every stack effect is known,
      // its signed sum is zero, and no interior branch/call can skip an effect.
      const balanced = region.every((s, i) => !s.unknown && !s.call && (!s.branch || i === region.length - 1)) &&
        region.reduce((sum, s) => sum + s.delta, 0) === 0;
      if (region.some(s => s.growth) && !balanced)
        f.unknown.push(`unresolved cyclic allocation: backward branch to ${step.target}`);
    }
    if (!f.instructions) f.unknown.push('no decoded instructions');
    const old = functions.get(f.name);
    if (!old) { functions.set(f.name, f); continue; }
    // COMDAT definitions can occur in several translation units. Use the largest
    // frame and union edges/limitations; this deliberately overapproximates variants.
    const largest = old.bytes >= f.bytes ? old : f;
    functions.set(f.name, {...largest, calls: [...old.calls, ...f.calls],
      notes: [...new Set([...old.notes, ...f.notes, 'COMDAT variants: max frame, union edges'])],
      unknown: [...new Set([...old.unknown, ...f.unknown])]});
  }
  if (!functions.size) throw new Error('No function symbols parsed: supply MSVC PROC/ENDP or dumpbin symbol labels');
  for (const f of functions.values()) for (const edge of f.calls) {
    const numeric = edge.target.match(/^([\da-f]{8,16})(?:\s|$)/i);
    if (numeric && !edge.indirect) edge.target = addresses.get(BigInt(`0x${numeric[1]}`).toString()) || edge.target;
  }
  return functions;
}

export function measure(text, roots = ROOTS) {
  const functions = parseListing(text);
  const selected = [], missing = [];
  for (const root of roots) {
    // Plain identifier roots also match MSVC decorated function names.
    const matches = [...functions.keys()].filter(n => n === root || n.startsWith(`?${root}@`));
    if (!matches.length) missing.push(root);
    for (const n of matches) if (!selected.includes(n)) selected.push(n);
  }
  const unresolved = new Set(), cycles = new Set(), reachable = new Set();
  const memo = new Map();
  // DFS memoization on a DAG; cycles are cut and reported as unbounded/unknown.
  function visit(name, active = []) {
    reachable.add(name);
    if (active.includes(name)) { cycles.add([...active, name].join(' -> ')); return {bytes: 0, chain: []}; }
    if (memo.has(name)) return memo.get(name);
    const f = functions.get(name);
    let best = {bytes: 0, chain: []};
    for (const edge of f.calls) {
      if (edge.indirect || edge.tail || !functions.has(edge.target)) {
        unresolved.add(`${name} -> ${edge.target}${edge.tail ? ' (tail jump)' : edge.indirect ? ' (indirect/virtual)' : ' (symbol absent)'}`);
        continue;
      }
      const child = visit(edge.target, [...active, name]);
      if (child.bytes > best.bytes) best = child;
    }
    const result = {bytes: f.bytes + best.bytes, chain: [name, ...best.chain]};
    memo.set(name, result);
    return result;
  }
  const chains = selected.map(root => ({root, ...visit(root)}));
  const drawChains = chains.filter(c => /Draw/i.test(c.root));
  const incomplete = missing.length || cycles.size || unresolved.size ||
    [...reachable].some(n => functions.get(n).unknown.length);
  // Apply the same ceiling to every supplied hook root, including Draw paths.
  const breached = chains.some(c => c.bytes >= BUDGET);
  const lines = [
    `Stack screening: chain < ${BUDGET} bytes; per-function warning > ${WARN} bytes`,
    'FUNCTION\tFRAME_BYTES\tPUSH_BYTES\tALLOCATION_BYTES\tNOTES'
  ];
  for (const f of [...functions.values()].sort((a,b) => a.name.localeCompare(b.name, 'en'))) {
    lines.push(`${f.name}\t${f.bytes}\t${f.pushes * 8}\t${f.allocation}\t${[
      f.bytes > WARN ? 'WARN >1 KiB' : '', f.allocation > 4096 ? 'allocation >4 KiB' : '',
      ...new Set(f.notes), ...f.unknown].filter(Boolean).join('; ')}`.trimEnd());
  }
  lines.push('DEEPEST KNOWN CHAINS (cycles cut; incomplete paths are not full bounds)');
  for (const c of chains) lines.push(`${c.root}: ${c.bytes} bytes: ${c.chain.join(' -> ')}`);
  if (drawChains.length) lines.push(`Deepest known Draw chain: ${Math.max(...drawChains.map(c => c.bytes))} bytes`);
  for (const root of missing) lines.push(`MISSING ROOT: ${root}`);
  for (const cycle of cycles) lines.push(`CYCLE (unbounded without depth limit): ${cycle}`);
  for (const edge of unresolved) lines.push(`UNRESOLVED: ${edge}`);
  lines.push(...LIMITS);
  const exitCode = breached ? 1 : incomplete ? 2 : 0;
  lines.push(`RESULT: ${breached ? 'BUDGET EXCEEDED' : incomplete ? 'INCOMPLETE (known chains below budget)' : 'within static listing budget'}; exit ${exitCode}`);
  return {output: lines.join('\n') + '\n', exitCode, functions, chains};
}

function selfTest() {
  const fixture = new URL('../../../tests/acz-vr/fixtures/stack-frames.asm', import.meta.url);
  const expected = new URL('../../../tests/acz-vr/fixtures/stack-frames.expected.txt', import.meta.url);
  const text = fs.readFileSync(fixture, 'utf8');
  const result = measure(text, ['hookedDrawInstanced']);
  assert.equal(result.output, fs.readFileSync(expected, 'utf8').replace(/\r\n/g, '\n'));
  assert.equal(result.exitCode, 2);
  assert.equal(result.functions.get('probe').bytes, 8208);
  assert.equal(result.chains[0].bytes, 8352);
  assert.equal(measure(text.replace('8192', '16384'), ['hookedDrawInstanced']).exitCode, 1);
  assert.equal(measure('leaf PROC\n sub rsp, 32\n ret\nleaf ENDP', ['leaf']).exitCode, 0);
  assert.equal(measure(text, ['missing']).exitCode, 2);
  const dump = measure('hookedDrawInstanced:\n  0000000140001000: 53 push rbx\n  0000000140001001: 48 83 EC 20 sub rsp,20h\n  0000000140001005: E8 00 00 00 00 call 0000000140002000\nleaf:\n  0000000140002000: C3 ret', ['hookedDrawInstanced']);
  assert.equal(dump.exitCode, 0);
  assert.equal(dump.chains[0].bytes, 56);
  assert.throws(() => parseListing('unlabelled garbage'), /No function/);
  const variants = measure('x PROC\n sub rsp, 32\n ret\nx ENDP\nx PROC\n sub rsp, 64\n ret\nx ENDP', ['x']);
  assert.equal(variants.functions.get('x').bytes, 72);
  assert.equal(variants.exitCode, 0);
  assert.equal(measure('x PROC\n sub rsp, rax\n ret\nx ENDP', ['x']).exitCode, 2);
  assert.equal(measure('x PROC\n call external\n ret\nx ENDP', ['x']).exitCode, 2);
  assert.equal(measure('x PROC\n lea rsp, [rbp-16]\n ret\nx ENDP', ['x']).exitCode, 2);
  assert.equal(measure('x PROC\n jmp external\nx ENDP', ['x']).exitCode, 2);
  const regressions = new URL('../../../tests/acz-vr/fixtures/stack-frames-regressions.json', import.meta.url);
  for (const c of JSON.parse(fs.readFileSync(regressions, 'utf8'))) {
    const input = fs.readFileSync(new URL(c.file, regressions), 'utf8');
    const result = measure(input, ['case']);
    assert.equal(result.exitCode, c.exitCode, c.note);
    assert.equal(result.output, fs.readFileSync(new URL(c.expected, regressions), 'utf8').replace(/\r\n/g, '\n'), c.note);
  }
  // Every intervening instruction (including alias/implicit writes) kills the cache.
  for (const instruction of ['pop rax', 'inc rax', 'dec eax', 'neg rax', 'not ax',
    'xchg rcx,rax', 'mov ax,32', 'mov al,32', 'mov ah,32', 'cmovne rax,rcx',
    'imul rax,rcx', 'mul rcx', 'div rcx', 'cdq', 'cqo', 'lea rax,[rcx]',
    'movzx eax,cl', 'movsx rax,cl', 'mov rdx,rax', 'rdtsc', 'rdtscp', 'nop']) {
    assert.equal(measure(`case PROC\nmov eax,32\n${instruction}\nsub rsp,rax\nret\ncase ENDP`, ['case']).exitCode, 2, instruction);
  }
  console.log('stack-frame self-test passed (synthetic MSVC and dumpbin fixtures; no real measurement)');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2);
    if (args.length === 1 && args[0] === '--self-test') selfTest();
    else {
      if (!args.length || args[0].startsWith('--')) throw new Error('Usage: node measure-stack-frames.mjs listing.asm [--roots name1,name2,...] | --self-test');
      if (args.length !== 1 && !(args.length === 3 && args[1] === '--roots' && args[2])) throw new Error('Expected optional --roots name1,name2,...');
      const result = measure(fs.readFileSync(args[0], 'utf8'), args.length === 3 ? args[2].split(',').map(s => s.trim()) : ROOTS);
      process.stdout.write(result.output);
      process.exitCode = result.exitCode;
    }
  } catch (error) { console.error(`Stack measurement failed: ${error.message}`); process.exitCode = 2; }
}
