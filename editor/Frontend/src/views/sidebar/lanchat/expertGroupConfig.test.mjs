import assert from 'node:assert/strict';

import {
  addCustomExpert,
  createExpertGroupConfig,
  selectedExpertPayloads,
  setRoleSelected,
} from './expertGroupConfig.js';

const roles = [
  { key: 'elder', name: '长者', persona: '长者' },
  { key: 'merchant', name: '商人', persona: '商人' },
  { key: 'girl', name: '小女孩', persona: '小女孩' },
];

const config = createExpertGroupConfig(roles, ['elder', 'girl']);
assert.deepEqual([...config.selectedRoleKeys].sort(), ['elder', 'girl']);

setRoleSelected(config, 'elder', false);
setRoleSelected(config, 'merchant', true);
addCustomExpert(config, { name: '灯光师', persona: '负责灯光氛围' });
addCustomExpert(config, { name: ' ', persona: 'ignored' });

assert.deepEqual(selectedExpertPayloads(config, roles), [
  { name: '商人', persona: '商人', roleKey: 'merchant' },
  { name: '小女孩', persona: '小女孩', roleKey: 'girl' },
  { name: '灯光师', persona: '负责灯光氛围' },
]);

console.log('[OK] lanchat expert group config builds selected agent payloads');
