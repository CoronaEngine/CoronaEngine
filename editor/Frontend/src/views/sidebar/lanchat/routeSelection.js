export function resolveSelectedTargetKey(currentKey, options = []) {
  const key = String(currentKey || '').trim();
  const list = Array.isArray(options) ? options : [];
  if (!key) return '';
  if (list.some((item) => item.key === key)) return key;
  return list[0]?.key || 'scene';
}

export function targetPayloadForKey(key, options = []) {
  const value = String(key || '').trim();
  if (!value) {
    return {
      scope: '',
      agentId: '',
      agentName: '',
      planId: '',
    };
  }
  const list = Array.isArray(options) ? options : [];
  const target = list.find((item) => item.key === value) || list[0] || {};
  return {
    scope: target.scope || 'scene',
    agentId: target.agentId || '',
    agentName: target.agentName || '',
    planId: target.planId || '',
  };
}

export function isGenerationStartText(text = '') {
  const value = String(text || '').trim();
  if (!value) return false;
  return /(确认开始|确认生成|开始生成|直接生成|开始执行|执行生成|按.*方案.*生成|按这个方案生成|就按方案生成)/.test(value);
}

export function effectiveDraftAction(action, text = '') {
  if (isGenerationStartText(text)) return 'generate';
  return String(action || '').trim();
}

export function targetKeyForMentionName(name, options = []) {
  const value = String(name || '').trim().toLowerCase();
  if (!value) return '';
  const list = Array.isArray(options) ? options : [];
  const target = list.find((item) => {
    const names = [item.label, item.agentName, item.agentId].map((part) => (
      String(part || '').trim().toLowerCase()
    ));
    return names.includes(value);
  });
  return target?.key || '';
}

export function targetKeyFromMentionText(text, options = []) {
  const pattern = /@([^\s，,：:]+)/g;
  const source = String(text || '');
  let match = pattern.exec(source);
  while (match) {
    const key = targetKeyForMentionName(match[1], options);
    if (key) return key;
    match = pattern.exec(source);
  }
  return '';
}

export function pendingReplyMatchesMessage(message = {}, pending = {}) {
  if (!pending || !pending.correlationId || !message) return false;
  const correlationId = String(message.correlation_id || message.metadata?.correlation_id || '').trim();
  if (correlationId !== pending.correlationId) return false;
  const kind = String(message.message_kind || '').toLowerCase();
  if (kind && !['agent_reply', 'gm_proposal', 'action_status'].includes(kind)) return false;
  const targetId = String(message.target_agent_id || '').trim().toLowerCase();
  const pendingTargetId = String(pending.targetAgentId || '').trim().toLowerCase();
  return !pendingTargetId || !targetId || targetId === pendingTargetId || pendingTargetId === 'gm';
}

export function isAiAssistantReply(message = {}) {
  const kind = String(message.message_kind || '').toLowerCase();
  if (kind !== 'agent_reply') return false;
  const senderType = String(message.sender_type || '').toLowerCase();
  return !['user', 'host', 'system'].includes(senderType);
}

function userMessageMatchesCorrelation(message = {}, correlationId = '') {
  if (!correlationId) return false;
  if (String(message.correlation_id || '').trim() !== correlationId) return false;
  const kind = String(message.message_kind || '').toLowerCase();
  if (kind && kind !== 'chat' && kind !== 'confirmation') return false;
  const senderType = String(message.sender_type || '').toLowerCase();
  return !['agent', 'gm', 'system'].includes(senderType);
}

export function displayNameForMemberId(memberId = '', memberDetails = [], messages = []) {
  const id = String(memberId || '').trim();
  if (!id) return '';
  const members = Array.isArray(memberDetails) ? memberDetails : [];
  const member = members.find((item) => String(item.member_id || item.id || '') === id);
  if (member?.nickname || member?.name) return member.nickname || member.name;
  const history = Array.isArray(messages) ? messages : [];
  const authored = history.find((item) => (
    String(item.sender_id || '').trim() === id &&
    !['agent', 'gm', 'system'].includes(String(item.sender_type || '').toLowerCase()) &&
    String(item.from || '').trim()
  ));
  return authored?.from || '';
}

export function aiReplyAddressedUserName(message = {}, messages = [], memberDetails = []) {
  if (!isAiAssistantReply(message)) return '';
  const sourceUserId = String(message.source_user_id || '').trim();
  const bySource = displayNameForMemberId(sourceUserId, memberDetails, messages);
  if (bySource) return bySource;

  const correlationId = String(message.correlation_id || '').trim();
  const sourceMessage = (Array.isArray(messages) ? messages : []).find((item) => (
    item !== message && userMessageMatchesCorrelation(item, correlationId)
  ));
  if (sourceMessage?.from) return sourceMessage.from;
  return '';
}

export function aiReplyDisplayText(message = {}, messages = [], memberDetails = []) {
  const text = String(message.text || '');
  const userName = aiReplyAddressedUserName(message, messages, memberDetails);
  return userName ? `@${userName} ${text}` : text;
}

export function displaySenderName(message = {}) {
  const name = String(message.from || '').trim();
  if (!name) return '';
  const senderType = String(message.sender_type || '').toLowerCase();
  const kind = String(message.message_kind || '').toLowerCase();
  const isAssistant = (
    (kind === 'agent_reply' && !['user', 'host', 'system'].includes(senderType)) ||
    senderType === 'agent' ||
    senderType === 'gm'
  );
  if (!isAssistant || name.startsWith('🤖')) return name;
  return `🤖${name}`;
}

export function routeGuardMessage(action, target = {}, text = '') {
  const value = String(text || '').trim();
  if (/^@([^\s，,：:]+)/.test(value)) return '';
  if (isGenerationStartText(value)) return '';
  const draftAction = String(action || '').trim();
  const scope = String(target?.scope || '').trim();
  if (draftAction === 'plan' && scope === 'group') {
    return '生成方案需要先选择一个负责整理方案的 Agent。';
  }
  if (draftAction === 'supplement' && scope !== 'agent' && scope !== 'plan') {
    return '补充要求需要选择已有方案对应的 Agent。';
  }
  if (draftAction === 'generate' && scope !== 'agent' && scope !== 'plan') {
    return '确认生成需要选择已有方案对应的 Agent。';
  }
  return '';
}
