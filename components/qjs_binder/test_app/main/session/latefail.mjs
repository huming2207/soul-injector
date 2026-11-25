import { delay } from 'si:target';

await delay(50);
throw new Error('late');
