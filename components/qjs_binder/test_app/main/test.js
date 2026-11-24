import * as t from 'si:target';
import * as flash from 'si:flash';
import * as crypto from 'si:crypto';
import * as log from 'si:log';
import * as job from 'si:job';

const progress = { n: 0, last: null };
job.onProgress((p) => {
    progress.n++;
    progress.last = p;
});

let topErr = '';
try {
    t.read32(0x1000);
} catch (e) {
    topErr = e.code;
}

let carried = 0;

async function codeOf(fn) {
    try {
        await fn();
    } catch (e) {
        return e.code ?? String(e);
    }
    return 'none';
}

export async function preProgram(ctx) {
    switch (ctx.job.name) {
    case 'ops': {
        if (topErr !== 'NOT_OWNED') throw new Error(`top level got ${topErr}`);
        if (ctx.hook !== 'preProgram' || ctx.job.family !== 'mock') throw new Error('ctx');
        t.write32(0x1000, 5);
        const v = t.rmw32(0x1000, 0xffffff00, 0xaa);
        if (v !== 0xaa || t.read32(0x1000) !== 0xaa) throw new Error('rmw');
        if ((await t.poll32(0x2000, 1, 1, { timeoutMs: 500 })) !== 1) throw new Error('poll value');
        t.halt();
        await t.reset();
        await t.reinit();
        await t.waitHalt();
        await t.delay(30);
        carried = 0x1234;
        return;
    }
    case 'throw':
        throw new Error('boom');
    case 'fail':
        throw new Error('boom2');
    case 'failfail':
        throw new Error('first');
    case 'regs': {
        t.halt();
        t.writeReg('r0', 5);
        t.writeReg('S3', 0x3f800000);
        t.writeReg(0x14, 7);
        if (t.readReg('R0') !== 5 || t.readReg(0x43) !== 0x3f800000 || t.readReg('cfbp') !== 7) throw new Error('regs');
        for (const bad of ['r16', 'foo', 128]) {
            const c = await codeOf(() => t.readReg(bad));
            if (!c.startsWith('RangeError')) throw new Error(`readReg(${bad}) gave ${c}`);
        }
        t.resume();
        const c = await codeOf(() => t.readReg('pc'));
        if (c !== 'NOT_HALTED') throw new Error(`readReg while running gave ${c}`);
        return;
    }
    case 'spin':
        for (;;) {}
    case 'forever':
        await new Promise(() => {});
        return;
    case 'slow': {
        const c = await codeOf(() => t.read32(0xdead));
        if (c !== 'TIMEOUT') throw new Error(`slow read gave ${c}`);
        if (t.read32(0x1000, { timeoutMs: 1000 }) !== 0x77) throw new Error('read after timeout');
        return;
    }
    case 'pollfail': {
        const c = await codeOf(() => t.poll32(0x3000, 1, 1, { timeoutMs: 50 }));
        if (c !== 'TIMEOUT') throw new Error(`poll gave ${c}`);
        return;
    }
    case 'busy': {
        const p = t.delay(100);
        const c = await codeOf(() => t.read32(0x1000));
        await p;
        if (c !== 'BUSY') throw new Error(`sync during async gave ${c}`);
        return;
    }
    case 'fault': {
        const c = await codeOf(() => t.write32(0xbad, 1, { retries: 2 }));
        if (c !== 'FAULT') throw new Error(`fault gave ${c}`);
        return;
    }
    case 'cancel':
        await t.delay(10000);
        return;
    case 'mem': {
        t.writeMem(0x20000000, new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]));
        const b = t.readMem(0x20000000, 8);
        if (b.join() !== '1,2,3,4,5,6,7,8') throw new Error(`readMem gave ${b.join()}`);
        const c = await codeOf(() => t.readMem(0x20000000, 5000));
        if (!c.startsWith('RangeError')) throw new Error(`big readMem gave ${c}`);
        return;
    }
    case 'flash': {
        await flash.erase(0x0803f800, 2048);
        await flash.write(0x0803f800, new Uint8Array([0xde, 0xad, 0xbe, 0xef]));
        const r = await flash.read(0x0803f800, 4);
        if (r.join() !== '222,173,190,239') throw new Error(`flash.read gave ${r.join()}`);
        if ((await flash.read(0x0803f800, 2)).join() !== '222,173') throw new Error('second read');
        const p1 = flash.read(0x0803f800, 4);
        const c = await codeOf(() => flash.write(0x0803f800, new Uint8Array(4)));
        await p1;
        if (c !== 'BUSY') throw new Error(`second buffer call gave ${c}`);
        return;
    }
    case 'call': {
        const r0 = await t.call(0x20001000, [1, 2, 3, 4], { stackTop: 0x20008000, timeoutMs: 200 });
        if (r0 !== 10) throw new Error(`call returned ${r0}`);
        const bad = await codeOf(() => t.call(0x20001000, [], { stackTop: 0x20008001 }));
        if (!bad.startsWith('TypeError')) throw new Error(`unaligned stack gave ${bad}`);
        const u = await codeOf(() => t.call(0xdead, [], { stackTop: 0x20008000 }));
        if (u !== 'UNSUPPORTED') throw new Error(`call on a target without it gave ${u}`);
        return;
    }
    case 'crypto': {
        const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
        const h = hex(crypto.sha256('abc'));
        if (h !== 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') throw new Error(`sha256 ${h}`);
        if (hex(crypto.sha256(new Uint8Array([0x61, 0x62, 0x63]))) !== h) throw new Error('sha256 of bytes');
        const a = crypto.randomBytes(16);
        const b = crypto.randomBytes(16);
        if (a.length !== 16 || hex(a) === hex(b)) throw new Error('randomBytes');
        return;
    }
    case 'log': {
        log.record({ serial: 'SN0001', fw: 3 });
        log.check('vdd', 3.31, { lo: 3.0, hi: 3.6, unit: 'V' });
        const c = await codeOf(() => log.record({ blob: 'x'.repeat(1000) }));
        if (!c.startsWith('RangeError')) throw new Error(`oversized record gave ${c}`);
        return;
    }
    }
}

export async function postProgram(ctx) {
    const name = ctx.job.name;
    if (name === 'ops' && carried !== 0x1234) throw new Error('state lost between hooks');
    if (name === 'log') log.check('idle_uA', 5, { hi: 1, unit: 'uA' }); // fails the hook
    if (name === 'progress') {
        const last = progress.last;
        if (!last || last.stage !== 'program' || last.pct !== 100) throw new Error(`last progress ${JSON.stringify(last)}`);
        if (progress.n < 2 || progress.n > 10) throw new Error(`${progress.n} progress callbacks`);
    }
    if (name.startsWith('net')) {
        let got = 'yes';
        try {
            await import('esp:ws');
        } catch (e) {
            got = 'no';
        }
        if (got !== (name === 'net-on' ? 'yes' : 'no')) throw new Error(`esp:ws import: ${got}`);
    }
}

export async function test(ctx) {
    if (ctx.hook !== 'test') throw new Error(`ctx.hook ${ctx.hook}`);
    log.record({ tested: 1 });
}

export async function onFail(ctx, stage, err) {
    if (ctx.hook !== 'onFail' || !(err instanceof Error)) throw new Error('onFail args');
    log.record({ stage, msg: err.message });
    if (ctx.job.name === 'failfail') throw new Error('second');
    t.write32(0x1000, 0xf);
}
