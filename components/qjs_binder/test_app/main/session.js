//--- add
1 + 2
//--- setx
var x = 5;
x
//--- usex
x * 2
//--- read
await target.read32(0x1000)
//--- throw
throw new Error('nope')
//--- write
await target.write32(0x1000, 1)
//--- types
[new Uint8Array([1, 2]), 10n, undefined, new Error('e')]
//--- undef
undefined
//--- console
console.log('hi there');
'logged'
//--- emit
agent.emit('ping', { a: 1 })
//--- slow
await target.delay(300);
7
//--- big
'x'.repeat(5000)
//--- reject
await Promise.reject(new TypeError('bad'))
//--- uncaught
target.delay(10).then(() => { throw new Error('late boom'); });
1
//--- perms
agent.permissions()
//--- waithalt
await target.waitHalt({ timeoutMs: 100 })
//--- watches
agent.watches().map((w) => w.on + ':' + w.reflex).join(',')
//--- seen
typeof seen === 'undefined' ? 'none' : seen.join(',')
//--- watchdup
target.watch({ on: 'halt' })
//--- resetreflex
target.watch({ on: 'reset', reflex: 'reset' })
//--- typeofx
typeof x
//--- spin
for (;;) {}
//--- watchers module
import { watch } from 'si:target';
import { emit } from 'si:agent';

globalThis.seen = [];
watch({ on: 'halt', name: 'bp', capture: { mem: [[0x20000000, 16]] } }, (ev) => {
    seen.push(ev.cause);
    emit('halted', { cause: ev.cause, pc: ev.pc, regPc: ev.regs.pc, mem: ev.mem.length, stack: ev.stack.data.length });
    return 'resume';
});
watch({ on: 'fault' }, async (ev) => {
    emit('fault', { cause: ev.cause, cfsr: ev.fault.cfsr });
});
//--- empty module
export const nothing = true;
//--- stuck module
import { watch } from 'si:target';

watch({ on: 'halt', handlerTimeoutMs: 100 }, () => new Promise(() => {}));
//--- bad module
import { nope } from 'si:target';
