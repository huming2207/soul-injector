import * as t from 'si:target';

const FLASH_KEYR = 0x58004008;
const FLASH_OPTKEYR = 0x5800400c;
const FLASH_SR = 0x58004010;
const FLASH_CR = 0x58004014;
const FLASH_OPTR = 0x58004020;

const CR_LOCK = 0x80000000;
const CR_OPTLOCK = 0x40000000;
const CR_OPTSTRT = 0x00020000;
const CR_OBL_LAUNCH = 0x08000000;
const SR_BSY = 0x00010000;

async function unlockOptionBytes() {
    t.write32(FLASH_KEYR, 0x45670123);
    t.write32(FLASH_KEYR, 0xcdef89ab);
    await t.poll32(FLASH_CR, CR_LOCK, 0, { timeoutMs: 100 });
    t.write32(FLASH_OPTKEYR, 0x08192a3b);
    t.write32(FLASH_OPTKEYR, 0x4c5d6e7f);
    await t.poll32(FLASH_CR, CR_OPTLOCK, 0, { timeoutMs: 100 });
    await t.poll32(FLASH_SR, SR_BSY, 0, { timeoutMs: 1000 });
}

async function setRdp(level, programMs) {
    t.halt();
    await unlockOptionBytes();
    t.rmw32(FLASH_OPTR, 0xffffff00, level);
    t.rmw32(FLASH_CR, ~CR_OPTSTRT >>> 0, CR_OPTSTRT);
    await t.poll32(FLASH_SR, SR_BSY, 0, { timeoutMs: programMs });
    try {
        t.rmw32(FLASH_CR, ~CR_OBL_LAUNCH >>> 0, CR_OBL_LAUNCH);
    } catch (e) {
        console.log(`OBL_LAUNCH: ${e.code}, expected while the target resets`);
    }
    await t.delay(500);
    await t.reinit();
}

export async function preProgram(ctx) {
    console.log(`${ctx.job.name}: RDP 0 (mass erase)`);
    await setRdp(0xaa, 10000);
}

export async function postProgram(ctx) {
    console.log(`${ctx.job.name}: RDP 1`);
    await setRdp(0xbb, 1000);
}
