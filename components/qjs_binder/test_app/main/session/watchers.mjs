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
