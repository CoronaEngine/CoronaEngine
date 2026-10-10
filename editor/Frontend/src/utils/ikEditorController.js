let nextDraftId = 0;

const finite = (value, fallback, minimum = -Infinity, maximum = Infinity) => {
  const number = value === '' || value == null ? NaN : Number(value);
  return Number.isFinite(number) ? Math.min(maximum, Math.max(minimum, number)) : fallback;
};

export function makeIkFoot(chain = {}) {
  const target = chain.target ?? [0, 0, 0];
  return {
    id: chain.id || `web-foot-${Date.now()}-${++nextDraftId}`,
    boneName: chain.bone_name ?? '',
    mode: chain.mode ?? 'foot_plant',
    chainLength: chain.chain_length ?? 3,
    weight: chain.weight ?? 1,
    targetX: target[0],
    targetY: target[1],
    targetZ: target[2],
    maxIterations: chain.max_iterations ?? 10,
    tolerance: chain.tolerance ?? 0.001,
    damping: chain.damping ?? 0.88,
    enabled: chain.enabled ?? true,
    probeMaxDrop: chain.probe_max_drop ?? 0.5,
    footHeight: chain.foot_height ?? 0,
    plantThreshold: chain.plant_threshold ?? 0.08,
    contactWeightRise: chain.contact_weight_rise ?? 8,
    contactWeightDecay: chain.contact_weight_decay ?? 0.8,
    contactNormalOffset: chain.contact_normal_offset ?? 0,
    runtime: {
      has_target: false,
      grounded: false,
      releasing: false,
      weight: 0,
      target: [0, 0, 0],
      normal: [0, 1, 0],
      ...chain.runtime,
    },
  };
}

export function isAutomaticIkFoot(foot) {
  return foot.mode === 'foot_plant' || foot.mode === 'contact';
}

export function ikBoneOptions(foot, state) {
  return foot.mode === 'foot_plant' ? state.footBoneNames : state.boneNames;
}

export function ikBoneError(foot, state) {
  if (foot.mode !== 'foot_plant' || !foot.boneName || state.footBoneNames.includes(foot.boneName)) return '';
  return `当前末端“${foot.boneName}”不是可用的叶子节点，请重新选择。原配置已保留。`;
}

function skeletonOptions(skeleton) {
  const nodes = Array.isArray(skeleton.nodes) ? skeleton.nodes : null;
  const names = nodes ? nodes.map((node) => node?.name) : skeleton.leaves;
  const counts = new Map();
  for (const name of Array.isArray(names) ? names : []) {
    if (typeof name === 'string' && name.trim()) counts.set(name, (counts.get(name) ?? 0) + 1);
  }
  const unique = (name) => counts.get(name) === 1;
  if (!nodes) {
    const leaves = [...counts.keys()].filter(unique);
    return { boneNames: leaves, footBoneNames: leaves };
  }
  const selectable = nodes.filter((node) => node && Number.isInteger(node.parent) && node.parent >= 0 && unique(node.name));
  return {
    boneNames: selectable.map((node) => node.name),
    footBoneNames: selectable.filter((node) => node.leaf === true).map((node) => node.name),
  };
}

export function ikFootStatus(foot) {
  if (!foot.enabled) return '已禁用';
  if (Number(foot.weight) === 0) return '无影响（混合上限为 0）';
  if (!isAutomaticIkFoot(foot)) return '手动位置目标';
  if (foot.runtime?.releasing) return '释放支撑中';
  if (foot.runtime?.grounded) return foot.mode === 'foot_plant' ? '支撑中' : '接触中';
  if (foot.runtime?.has_target && foot.runtime.weight > 0) return '淡出中';
  return foot.mode === 'foot_plant' ? '等待地面' : '等待接触';
}

export function serializeIkFeet(feet) {
  return feet
    .filter((foot) => foot.boneName)
    .map((foot) => ({
      id: foot.id,
      bone_name: foot.boneName,
      mode: foot.mode,
      chain_length: Math.trunc(finite(foot.chainLength, 3, 2, 64)),
      weight: finite(foot.weight, 1, 0, 1),
      target: [foot.targetX, foot.targetY, foot.targetZ].map((value) => finite(value, 0)),
      max_iterations: Math.trunc(finite(foot.maxIterations, 10, 1, 128)),
      tolerance: finite(foot.tolerance, 0.001, 0.000001, 1),
      damping: finite(foot.damping, 0.88, 0, 1),
      enabled: Boolean(foot.enabled),
      probe_max_drop: finite(foot.probeMaxDrop, 0.5, 0, 100),
      foot_height: finite(foot.footHeight, 0, 0, 10),
      plant_threshold: finite(foot.plantThreshold, 0.08, 0, 100),
      contact_weight_rise: finite(foot.contactWeightRise, 8, 0, 100),
      contact_weight_decay: finite(foot.contactWeightDecay, 0.8, 0, 100),
      contact_normal_offset: finite(foot.contactNormalOffset, 0, -100, 100),
    }));
}

export function createIkEditorState() {
  return {
    isSkinned: false,
    ready: false,
    loading: false,
    saving: false,
    revision: 0,
    boneNames: [],
    footBoneNames: [],
    feet: [],
    error: '',
  };
}

function resultData(raw) {
  const data = raw?.data ?? raw ?? {};
  if (data.status === 'error' || data.ok === false || data.error) {
    throw new Error(data.message || data.error || 'IK 请求失败');
  }
  return data;
}

// Each selection owns both its reads and its write queue. In-flight CEF requests
// cannot be cancelled, but their responses can never mutate a newer selection.
export function createIkEditorController(api, state) {
  let active = null;

  const clear = () => {
    active = null;
    Object.assign(state, createIkEditorState());
  };

  async function select(sceneName, actorName) {
    clear();
    if (!sceneName || !actorName) return;
    const session = { sceneName, actorName, revision: 0, pending: null, flushing: null };
    active = session;
    state.loading = true;
    try {
      const [skeletonRaw, configRaw] = await Promise.all([
        api.getActorSkeletonLeaves(sceneName, actorName),
        api.getActorIkChains(sceneName, actorName),
      ]);
      if (active !== session) return;
      const skeleton = resultData(skeletonRaw);
      const config = resultData(configRaw);
      if (config.ready !== true) throw new Error('IK 数据尚未就绪，请刷新后重试');
      if (!Number.isSafeInteger(config.revision) || config.revision < 0) {
        throw new Error('IK 配置版本无效，请刷新后重试');
      }
      state.isSkinned = Boolean(config.is_skinned);
      Object.assign(state, skeletonOptions(skeleton));
      state.feet = (config.chains ?? []).map(makeIkFoot);
      state.revision = session.revision = config.revision;
      state.ready = true;
    } catch (error) {
      if (active === session) state.error = error?.message || '读取 IK 配置失败';
    } finally {
      if (active === session) state.loading = false;
    }
  }

  async function drain(session) {
    state.saving = true;
    try {
      while (active === session && session.pending !== null) {
        const chains = session.pending;
        session.pending = null;
        const response = resultData(
          await api.setActorIkChains(session.sceneName, session.actorName, chains, session.revision)
        );
        if (active !== session) return;
        if (!Number.isSafeInteger(response.revision) || response.revision <= session.revision) {
          throw new Error('IK 更新未返回有效版本，请刷新后重试');
        }
        state.revision = session.revision = response.revision;
        // Preserve newer local edits, including empty draft cards. Only runtime
        // observations are merged; the next payload still uses user settings.
        const byId = new Map((response.chains ?? []).map((chain) => [chain.id, chain]));
        for (const foot of state.feet) {
          const updated = byId.get(foot.id);
          if (updated) foot.runtime = updated.runtime;
        }
      }
    } catch (error) {
      if (active === session) {
        session.pending = null;
        state.ready = false;
        state.error = `${error?.message || '更新 IK 配置失败'}。请刷新配置后重试。`;
      }
    } finally {
      session.flushing = null;
      if (active === session) state.saving = false;
    }
  }

  function apply() {
    if (!active || !state.ready || !state.isSkinned) return Promise.resolve();
    const invalidFoot = state.feet.find((foot) => ikBoneError(foot, state));
    if (invalidFoot) {
      active.pending = null;
      state.error = ikBoneError(invalidFoot, state);
      return active.flushing ?? Promise.resolve();
    }
    state.error = '';
    active.pending = serializeIkFeet(state.feet);
    if (!active.flushing) active.flushing = drain(active);
    return active.flushing;
  }

  async function refresh() {
    const session = active;
    if (!session) return;
    if (session.flushing) await session.flushing;
    if (active === session) await select(session.sceneName, session.actorName);
  }

  return { select, apply, refresh, clear };
}
