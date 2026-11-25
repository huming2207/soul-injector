import { watch } from 'si:target';
import { emit } from 'si:agent';

const hist = [1, 2, 3];
watch({ on: 'halt', name: 'bp' }, (ev) => {
    emit('inc', { id: ev.incidentId });
    return { action: 'resume', data: { hist } };
});
watch({ on: 'fault', reflex: 'none' });
