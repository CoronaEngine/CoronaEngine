import assert from 'node:assert/strict';

import {
  effectiveDraftAction,
  isGenerationStartText,
  resolveSelectedTargetKey,
  routeGuardMessage,
  targetPayloadForKey,
} from './routeSelection.js';

const options = [
  { key: 'agent:elder', label: '长者', scope: 'agent', agentId: 'agent-elder', agentName: '长者' },
  { key: 'agent:girl', label: '小女孩', scope: 'agent', agentId: 'agent-girl', agentName: '小女孩' },
  { key: 'agent:merchant', label: '商人', scope: 'agent', agentId: 'agent-merchant', agentName: '商人' },
  { key: 'group', label: '专家组', scope: 'group' },
  { key: 'scene', label: '当前场景', scope: 'scene' },
];

assert.equal(resolveSelectedTargetKey('scene', options), 'scene');
assert.equal(resolveSelectedTargetKey('agent:merchant', options), 'agent:merchant');
assert.equal(resolveSelectedTargetKey('missing', options), 'agent:elder');

assert.deepEqual(targetPayloadForKey('agent:merchant', options), {
  scope: 'agent',
  agentId: 'agent-merchant',
  agentName: '商人',
  planId: '',
});

assert.deepEqual(targetPayloadForKey('scene', options), {
  scope: 'scene',
  agentId: '',
  agentName: '',
  planId: '',
});

assert.equal(routeGuardMessage('chat', options[0]), '');
assert.equal(isGenerationStartText('确认开始'), true);
assert.equal(isGenerationStartText('开始生成'), true);
assert.equal(isGenerationStartText('直接生成'), true);
assert.equal(isGenerationStartText('新增一个柜式空调'), false);
assert.equal(effectiveDraftAction('chat', '新增一个柜式空调'), 'chat');
assert.equal(effectiveDraftAction('chat', '确认开始'), 'generate');
assert.equal(effectiveDraftAction('chat', '开始生成'), 'generate');
assert.equal(
  routeGuardMessage('chat', { key: 'group', label: '专家组', scope: 'group' }, '@长者 帮我设计客厅'),
  ''
);
assert.equal(routeGuardMessage('chat', { key: 'scene', label: '当前场景', scope: 'scene' }, '整理一下方案'), '');

console.log('[OK] lanchat route selection keeps explicit target stable');
