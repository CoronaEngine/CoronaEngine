export const HOST_MEMBER_RAW_NAME = '房主';

export const BUILTIN_AGENT_ROLES = {
  elder: {
    canonicalName: '长者',
    canonicalPersona: '长者',
    nameKey: 'lanchat.roleElder',
    hintKey: 'lanchat.roleElderHint',
    aliases: ['Elder'],
  },
  little_girl: {
    canonicalName: '小女孩',
    canonicalPersona: '小女孩',
    nameKey: 'lanchat.roleLittleGirl',
    hintKey: 'lanchat.roleLittleGirlHint',
    aliases: ['Little Girl'],
  },
  bandit: {
    canonicalName: '山贼',
    canonicalPersona: '山贼',
    nameKey: 'lanchat.roleBandit',
    hintKey: 'lanchat.roleBanditHint',
    aliases: ['Bandit'],
  },
  scholar: {
    canonicalName: '学者',
    canonicalPersona: '学者',
    nameKey: 'lanchat.roleScholar',
    hintKey: 'lanchat.roleScholarHint',
    aliases: ['Scholar'],
  },
  merchant: {
    canonicalName: '商人',
    canonicalPersona: '商人',
    nameKey: 'lanchat.roleMerchant',
    hintKey: 'lanchat.roleMerchantHint',
    aliases: ['Merchant'],
  },
};

function trim(value) {
  return String(value || '').trim();
}

export function inferBuiltinAgentRoleKey(agentOrName = {}) {
  const explicitRoleKey = trim(agentOrName.roleKey || agentOrName.role_key);
  if (Object.hasOwn(BUILTIN_AGENT_ROLES, explicitRoleKey)) return explicitRoleKey;

  const name = trim(typeof agentOrName === 'string'
    ? agentOrName
    : (agentOrName.name || agentOrName.agent_name));
  const persona = trim(typeof agentOrName === 'string' ? '' : agentOrName.persona);

  for (const [roleKey, role] of Object.entries(BUILTIN_AGENT_ROLES)) {
    if (name === role.canonicalName || role.aliases.includes(name)) return roleKey;
    if (persona === role.canonicalPersona || role.aliases.includes(persona)) return roleKey;
  }
  return '';
}

export function displayMemberName(memberOrName, t) {
  const name = trim(typeof memberOrName === 'string'
    ? memberOrName
    : (memberOrName.nickname || memberOrName.name));
  if (!name) return '';
  return name === HOST_MEMBER_RAW_NAME ? t('lanchat.hostMember') : name;
}

export function displayBuiltinAgentRoleName(roleKey, t) {
  const role = BUILTIN_AGENT_ROLES[roleKey];
  return role ? t(role.nameKey) : '';
}

export function displayBuiltinAgentRoleHint(roleKey, t) {
  const role = BUILTIN_AGENT_ROLES[roleKey];
  return role ? t(role.hintKey) : '';
}

export function displayAgentName(agentOrName, t) {
  const roleKey = inferBuiltinAgentRoleKey(agentOrName);
  if (roleKey) return displayBuiltinAgentRoleName(roleKey, t);
  return trim(typeof agentOrName === 'string'
    ? agentOrName
    : (agentOrName.name || agentOrName.agent_name));
}

export function displayChatName(name, t) {
  const memberName = displayMemberName(name, t);
  if (memberName !== trim(name)) return memberName;
  return displayAgentName(name, t) || memberName;
}
