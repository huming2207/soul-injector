import { watch } from 'si:target';

watch({ on: 'halt', handlerTimeoutMs: 100 }, () => new Promise(() => {}));
