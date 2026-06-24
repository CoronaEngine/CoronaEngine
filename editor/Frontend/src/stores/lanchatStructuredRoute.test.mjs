import assert from 'node:assert/strict';

import { lanChatService } from '../utils/bridge.js';
import { lanchat } from './lanchat.js';

let sent = null;

lanChatService.startLocalRoom = async ({ room, mode, nickname }) => ({
  ok: true,
  room,
  mode,
  you: nickname || '房主',
  peer_id: 'host-a',
  members: [nickname || '房主'],
  agents: [],
});

lanChatService.stopLocalRoom = async () => ({ ok: true });
lanChatService.sendMessage = async (text, options) => {
  sent = { text, options };
  return { ok: true };
};

await lanchat.openLocalRoom({ room: 'single-default', nickname: '房主' });
lanchat.handleEvent({
  channel: 'lanchat',
  event: 'agent_roster',
  agents: [
    { agent_id: 'agent-girl', name: '小女孩' },
    { agent_id: 'agent-merchant', name: '商人' },
  ],
});
lanchat.setWorkspaceMode('solo_single_agent');
assert.equal(lanchat.state.activeTarget.scope, 'agent');
assert.equal(lanchat.state.activeTarget.agentName, '设计助手');
lanchat.setDraftAction('plan');
lanchat.setActiveTarget({
  scope: 'agent',
  agentId: 'agent-girl',
  agentName: '小女孩',
});

await lanchat.sendMessage('帮我设计一个可爱的卧室');

assert.equal(sent.text, '帮我设计一个可爱的卧室');
assert.equal(sent.options.metadata.workspace_mode, 'solo_single_agent');
assert.equal(sent.options.metadata.draft_action, 'plan');
assert.equal(sent.options.metadata.target_scope, 'agent');
assert.equal(sent.options.metadata.target_agent_id, 'agent-girl');
assert.equal(sent.options.metadata.target_agent_name, '小女孩');

lanchat.setWorkspaceMode('solo_multi_agent');
lanchat.setDraftAction('chat');
lanchat.setActiveTarget({ scope: 'group' });
await lanchat.sendMessage('大家怎么看这个卧室？');

assert.equal(sent.options.metadata.workspace_mode, 'solo_multi_agent');
assert.equal(sent.options.metadata.draft_action, 'chat');
assert.equal(sent.options.metadata.target_scope, 'group');
assert.deepEqual(sent.options.metadata.target_agent_ids, ['agent-girl', 'agent-merchant']);
assert.deepEqual(sent.options.metadata.target_agent_names, ['小女孩', '商人']);

lanchat.setDraftAction('generate');
lanchat.setActiveTarget({ scope: 'plan', planId: 'plan-123', agentName: '小女孩' });
await lanchat.sendMessage('就按这个执行');

assert.equal(sent.options.metadata.draft_action, 'generate');
assert.equal(sent.options.metadata.target_scope, 'plan');
assert.equal(sent.options.metadata.target_plan_id, 'plan-123');
assert.equal(sent.options.metadata.target_agent_name, '小女孩');

await lanchat.closeRoom();
console.log('[OK] lanchat store sends structured route metadata');
