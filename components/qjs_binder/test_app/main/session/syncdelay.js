const p = target.delay(100);
const v = target.read32(0);
await p;
v;
