/** Pure geometry + acknowledged gameplay commands. No per-frame native queries. */
import { hasUsableBounds, rotatePoint, vector3 } from './storyCharacters.mjs';

export const GAMEPLAY_KEY = '__corona_story_gameplay_v1__';
export const FRAGMENT_GUID = 'f9f5c8b0-7324-4b6c-a011-000000000005';
export const FRAGMENT = Object.freeze({ role: 'fragment', name: '世界碎片', guid: FRAGMENT_GUID,
  asset: 'fragment/Ball.obj', x: 0, z: 12, size: 0.5, rotation: [0, 0, 0] });

export function worldBounds(actor) {
  const { position, rotation, scale } = actor?.geometry || {};
  if (!hasUsableBounds(actor?.local_aabb) || ![position, rotation, scale].every(vector3)) return null;
  const points = [];
  for (let i = 0; i < 8; i++) points.push(rotatePoint([0, 1, 2].map(axis =>
    actor.local_aabb[axis + ((i & (1 << axis)) ? 3 : 0)] * scale[axis]), rotation)
    .map((value, axis) => value + position[axis]));
  return [...[0, 1, 2].map(axis => Math.min(...points.map(p => p[axis]))),
    ...[0, 1, 2].map(axis => Math.max(...points.map(p => p[axis])))];
}

// Grounded kinematic actors use horizontal distance; their origins need not be at their feet.
export function distanceToBounds(position, bounds) {
  if (!vector3(position) || !hasUsableBounds(bounds)) return Infinity;
  return Math.hypot(...[0, 2].map(axis => Math.max(bounds[axis] - position[axis], 0, position[axis] - bounds[axis + 3])));
}
export const canAct = player => Boolean(player?.grounded && player.movementState !== 'dodging');
export function canHitBoss(player, bounds, config) {
  if (!canAct(player) || !Number.isFinite(player.facingYaw)
    || distanceToBounds(player.position, bounds) > config.meleeRange) return false;
  const dx = (bounds[0] + bounds[3]) / 2 - player.position[0];
  const dz = (bounds[2] + bounds[5]) / 2 - player.position[2];
  const length = Math.hypot(dx, dz);
  return length < 1e-8 || (Math.sin(player.facingYaw) * dx + Math.cos(player.facingYaw) * dz)
    / length >= Math.cos(config.meleeHalfAngle) - 1e-9;
}
export function pickupDistance(player, drop) {
  return canAct(player) && vector3(drop?.position)
    ? Math.hypot(player.position[0] - drop.position[0], player.position[2] - drop.position[2]) : Infinity;
}
export function unwrapGameplay(value) {
  for (let i = 0; i < 3 && value?.data; i++) value = value.data;
  // The CEF scratch bridge answers a key event with `{ response: '<json string>' }`,
  // which is the handler's own reply. Unwrap and parse it so callers see the payload.
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    if (Array.isArray(value.errors) && value.errors.length) {
      throw new Error(String(value.errors[0]?.message || value.errors[0]));
    }
    if (typeof value.response === 'string') value = value.response;
  }
  if (typeof value !== 'string') return value;
  try { return JSON.parse(value); } catch { return value; }
}

export function createStoryGameplay({ api, projectPath, readPlayer, readBoss,
  reconcile = async () => {}, onState = () => {}, onFeedback = () => {},
  trackWork = promise => promise, now = () => performance.now(), newId = () => crypto.randomUUID(),
  requestTimeoutMs = 15_000 }) {
  let data = null, pending = null, inFlight = null, visualDirty = false, lastAttack = -Infinity;
  // The small world's exhibit layout is its own document: it is not part of the
  // combat state, so it is cached separately and never merged into `data.state`.
  let placementCache = null;
  const skillUntil = { heavy: -Infinity, sweep: -Infinity };
  async function request(payload) {
    let timer;
    try {
      // The existing CEF bridge has no response timeout. Only this bounded RPC
      // feeds state: a late transport reply cannot overwrite a confirmed retry.
      // Retrying the same operation ID is safe even if its first reply is lost.
      return await Promise.race([
        trackWork(api.scratch.sendKeyEvent(GAMEPLAY_KEY, '', JSON.stringify({ projectPath, ...payload }))),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('玩法请求超时，结果尚未确认，请重试')), requestTimeoutMs);
        }),
      ]);
    } finally { clearTimeout(timer); }
  }
  function accept(response) {
    if (!response?.state || !response.config || !['main', 'child'].includes(response.role)) {
      throw new Error('玩法状态响应无效');
    }
    data = { state: response.state, config: response.config, role: response.role };
    onState(data);
  }
  async function applyVisuals() {
    if (!visualDirty) return;
    await reconcile(data);
    visualDirty = false;
  }
  function flush() {
    if (inFlight) return inFlight;
    const work = (async () => {
      if (pending) {
        const operation = pending;
        const response = unwrapGameplay(await request(operation));
        if (response?.code === 'REVISION_CONFLICT') {
          accept(response); pending = null; visualDirty = true;
          await applyVisuals();
          throw new Error(response.message);
        }
        if (response?.status !== 'ok') throw new Error(response?.message || '保存玩法进度失败');
        accept(response);
        pending = null; visualDirty = true;
        // Already committed; visual retry must not resend or grant the reward again.
        await applyVisuals();
        if (operation.action === 'pickupDrop') onFeedback('获得 世界碎片 ×1');
        else if (data.state.boss.hp === 0 && (operation.action === 'hitBoss' || operation.hit)) {
          onFeedback('Boss 已击败 · 小世界已开启');
        }
      } else await applyVisuals();
    })();
    inFlight = trackWork(work);
    inFlight.then(() => { inFlight = null; }, () => { inFlight = null; });
    return inFlight;
  }
  function commit(action, fields) {
    pending = { action, operationId: newId(), expectedRevision: data.state.revision, ...fields };
    return flush();
  }
  return {
    get data() { return data; },
    get busy() { return Boolean(inFlight); },
    get needsSave() { return Boolean(pending || visualDirty); },
    async load() {
      const response = unwrapGameplay(await request({ action: 'load' }));
      if (response?.status !== 'ok') throw new Error(response?.message || '读取玩法进度失败');
      accept(response);
      // A main world reports an empty layout, so one await covers both worlds.
      try { await loadPlacements(); } catch { placementCache = null; }
      return data;
    },
    // Exhibits are authored one write at a time and never share the combat revision,
    // so a rejected layout can never corrupt a boss or drop reward.
    async loadPlacements() {
      const response = unwrapGameplay(await request({ action: 'loadPlacements' }));
      if (response?.status !== 'ok' || !response.state?.placements) {
        throw new Error(response?.message || '读取小世界陈列失败');
      }
      return (placementCache = response.state);
    },
    async savePlacements(placed) {
      const response = unwrapGameplay(await request({ action: 'savePlacements', placements: placed }));
      if (response?.status !== 'ok' || !response.state?.placements) {
        throw new Error(response?.message || '保存小世界陈列失败');
      }
      // The scene must follow the confirmed layout, so mark the visuals dirty and let
      // the same reconcile path that handles combat redraw the exhibits.
      placementCache = response.state;
      visualDirty = true;
      return placementCache;
    },
    get placements() { return placementCache; },
    attack() {
      if (inFlight) return inFlight;
      if (pending || visualDirty) return flush();
      if (!data || data.role !== 'main' || data.state.boss.hp <= 0 || !canAct(readPlayer())) return Promise.resolve(false);
      const time = now(), boss = readBoss(), bounds = worldBounds(boss);
      if (time - lastAttack < data.config.cooldownMs) return Promise.resolve(false);
      lastAttack = time;
      if (!canHitBoss(readPlayer(), bounds, data.config)) {
        onFeedback('靠近并面向 Boss 后攻击'); return Promise.resolve(false);
      }
      return commit('hitBoss', { bossPosition: [boss.geometry.position[0], 0, boss.geometry.position[2]] });
    },
    skillStatus(skillId) {
      const skill = data?.config.skills?.[skillId];
      if (!skill) return { available: false, remainingMs: 0, reason: '未就绪' };
      const remainingMs = Math.max(0, skillUntil[skillId] - now());
      const reason = data.role !== 'main' ? '小世界禁用' : remainingMs > 0 ? '冷却中'
        : data.state.rage < skill.rageCost ? '怒气不足' : !canAct(readPlayer()) ? '动作中' : '';
      return { available: !reason && !pending && !visualDirty && !inFlight
        && now() - lastAttack >= data.config.cooldownMs, remainingMs, reason };
    },
    castSkill(skillId) {
      if (inFlight) return inFlight;
      if (pending || visualDirty) return flush();
      const skill = data?.config.skills?.[skillId];
      if (!skill || data.role !== 'main' || !canAct(readPlayer())) return Promise.resolve(false);
      const time = now();
      if (time - lastAttack < data.config.cooldownMs || time < skillUntil[skillId]) return Promise.resolve(false);
      if (data.state.rage < skill.rageCost) { onFeedback('怒气不足'); return Promise.resolve(false); }
      const boss = readBoss();
      const hit = data.state.boss.hp > 0 && canHitBoss(readPlayer(), worldBounds(boss),
        { meleeRange: skill.range, meleeHalfAngle: skill.halfAngle });
      lastAttack = time;
      skillUntil[skillId] = time + skill.cooldownMs;
      return commit('castSkill', { skillId, hit,
        ...(hit ? { bossPosition: [boss.geometry.position[0], 0, boss.geometry.position[2]] } : {}) });
    },
    pickup() {
      if (inFlight) return inFlight;
      if (pending || visualDirty) return flush();
      const drop = data?.state.drop;
      if (data?.role !== 'main' || !drop || drop.collected
        || pickupDistance(readPlayer(), drop) > data.config.pickupRange) return Promise.resolve(false);
      return commit('pickupDrop', { dropId: drop.id });
    },
    flush,
  };
}
