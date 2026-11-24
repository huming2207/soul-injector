// One script, one scenario per job name (ctx.job.name)
import * as t from 'si:target';

// The top level runs while loading: no target yet
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
    case 'spin':
        for (;;) {}
    case 'forever':
        await new Promise(() => {});
        return;
    case 'slow': {
        // 0xdead takes 600 ms, the call gives up at 200 ms
        const c = await codeOf(() => t.read32(0xdead));
        if (c !== 'TIMEOUT') throw new Error(`slow read gave ${c}`);
        // queued behind the slow one; its stale answer must not be taken for ours
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
    }
}

export async function postProgram(ctx) {
    if (ctx.job.name === 'ops' && carried !== 0x1234) throw new Error('state lost between hooks');
}
