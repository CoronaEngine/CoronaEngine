export function createExpertGroupConfig(roles = [], defaultKeys = []) {
  const availableKeys = new Set((roles || []).map((role) => role.key).filter(Boolean));
  const selectedRoleKeys = new Set(
    (defaultKeys || []).filter((key) => availableKeys.has(key))
  );
  return {
    selectedRoleKeys,
    customExperts: [],
  };
}

export function setRoleSelected(config, key, selected) {
  if (!config?.selectedRoleKeys || !key) return;
  if (selected) {
    config.selectedRoleKeys.add(key);
  } else {
    config.selectedRoleKeys.delete(key);
  }
}

export function addCustomExpert(config, expert = {}) {
  if (!config) return;
  const name = String(expert.name || '').trim();
  if (!name) return;
  const persona = String(expert.persona || name).trim() || name;
  if (!Array.isArray(config.customExperts)) config.customExperts = [];
  config.customExperts.push({ name, persona });
}

export function removeCustomExpert(config, index) {
  if (!Array.isArray(config?.customExperts)) return;
  config.customExperts.splice(index, 1);
}

export function selectedExpertPayloads(config, roles = []) {
  const selected = config?.selectedRoleKeys || new Set();
  const payloads = [];
  const seenNames = new Set();
  for (const role of roles || []) {
    if (!selected.has(role.key)) continue;
    const name = String(role.name || '').trim();
    if (!name || seenNames.has(name)) continue;
    seenNames.add(name);
    payloads.push({
      name,
      persona: String(role.persona || name).trim() || name,
      roleKey: role.key || '',
    });
  }
  for (const expert of config?.customExperts || []) {
    const name = String(expert.name || '').trim();
    if (!name || seenNames.has(name)) continue;
    seenNames.add(name);
    payloads.push({
      name,
      persona: String(expert.persona || name).trim() || name,
    });
  }
  return payloads;
}
