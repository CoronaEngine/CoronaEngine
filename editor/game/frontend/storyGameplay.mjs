/** Pure geometry + acknowledged gameplay commands. No per-frame native queries. */
import { hasUsableBounds, rotatePoint, vector3 } from './storyCharacters.mjs';
// The prophet's reach lives in `prophetDialogue`; this module owns the arithmetic that
// combines it with the block reach, so the F key's one decision never re-derives a distance.
import { PROPHET_INTERACTION } from './prophetDialogue.mjs';
// The block's reach belongs to the placement geometry, not to the combat config: the resolver
// reads it from the module that owns where a block lands, so placing and retrieving stay one
// pair of numbers and no caller has to re-state the reach.
import { PLACED_BLOCK_REACH, findProp } from './storyProps.mjs';
import { monsterEquipmentName } from './storyLoot.mjs';

export const GAMEPLAY_KEY = '__corona_story_gameplay_v1__';
// Kept only so a world saved by the old flow can hide the red ball actor it once
// created. The current game never creates it: the boss reward goes straight into the bag.
export const FRAGMENT_GUID = 'f9f5c8b0-7324-4b6c-a011-000000000005';

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
/**
 * The nearest attackable thing in front of the player: the boss and every living monster
 * are candidates, and the one whose bounding box is closest wins. The caller supplies the
 * range and half-angle, so a normal swing and a skill share this one rule.
 */
export function nearestAttackTarget(player, targets = [], config = {}) {
  if (!canAct(player)) return null;
  let best = null;
  for (const target of Array.isArray(targets) ? targets : []) {
    if (!target?.bounds || !canHitBoss(player, target.bounds, config)) continue;
    const distance = distanceToBounds(player.position, target.bounds);
    if (!best || distance < best.distance) best = { ...target, distance };
  }
  return best;
}
/**
 * The one rule the F key follows, shared by the on-screen prompt and the key handler.
 *
 * Each candidate carries its own configured range as a distance: the prophet's comes from
 * `PROPHET_INTERACTION.range` (defaulted here so a caller cannot forget it), and the placed
 * block's from `PLACED_BLOCK_REACH` (the reach the placement geometry itself owns, so
 * retrieving is the mirror of placing). The prompt text is produced beside the decision, so a
 * prompt can never describe a different action than the one F performs. The nearer target wins;
 * an exact tie keeps the earlier candidate — the prophet first, as it always was, which is what
 * stops a block lying next to the prophet from stealing the conversation. Pure: no engine, no DOM.
 */
export const INTERACTION_PROMPTS = Object.freeze({
  prophet: 'F 与先知交谈',
  block: '按 F 取回方块',
});
export function contextualInteraction({ prophetDistance: toProphet = Infinity,
  blockDistance: toBlock = Infinity,
  range = PROPHET_INTERACTION.range,
  blockRange = PLACED_BLOCK_REACH } = {}) {
  const candidates = [
    { kind: 'prophet', distance: toProphet, limit: range },
    { kind: 'block', distance: toBlock, limit: blockRange },
  ].filter(candidate => Number.isFinite(candidate.distance)
    && Number.isFinite(candidate.limit) && candidate.distance <= candidate.limit);
  if (!candidates.length) return { kind: null, distance: null, prompt: null };
  const nearest = candidates.reduce((best, candidate) =>
    candidate.distance < best.distance ? candidate : best);
  return { kind: nearest.kind, distance: nearest.distance, prompt: INTERACTION_PROMPTS[nearest.kind] };
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

export function createStoryGameplay({ api, projectPath, readPlayer, readBoss, readMonsters = () => [],
  reconcile = async () => {}, onState = () => {}, onFeedback = () => {},
  trackWork = promise => promise, now = () => performance.now(), newId = () => crypto.randomUUID(),
  requestTimeoutMs = 15_000 }) {
  let data = null, pending = null, inFlight = null, visualDirty = false, lastAttack = -Infinity;
  // The small world's exhibit layout is its own document: it is not part of the
  // combat state, so it is cached separately and never merged into `data.state`.
  let placementCache = null;
  let ruleCache = null;
  // The roster (named small worlds the player owns) is likewise its own document,
  // cached separately so a rename never rides on a combat revision.
  let rosterCache = null;
  // Why a side document could not be read. These documents are independent of the combat
  // revision, so a broken one must not block entry — but it must not disappear either.
  // An empty `catch` once hid a ReferenceError that left all three unread on every entry,
  // which is exactly how a reopening small world came back with an empty layout and no
  // installed fragment. Recording the reason is what keeps such a failure visible.
  const loadFailures = [];
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
        const before = data?.state || null;
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
        if (operation.monsterId && before?.monsters?.[operation.monsterId]?.hp > 0
          && data.state.monsters?.[operation.monsterId]?.hp === 0) {
          const gained = Object.keys(data.state.inventory || {})
            .map(key => ({ key, amount: (data.state.inventory[key] || 0)
              - (before.inventory?.[key] || 0) }))
            .filter(entry => entry.amount > 0)
            .map(({ key, amount }) =>
              `${findProp(key)?.name || monsterEquipmentName(key)} ×${amount}`);
          onFeedback(gained.length ? `击败怪物 · 获得 ${gained.join('、')}` : '击败怪物');
        } else if (!operation.monsterId && data.state.boss.hp === 0
          && (operation.action === 'hitBoss' || operation.hit)) {
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
  // 背包的三件事（披挂 / 服用 / 弃置）都只是一条后端操作：界面只管发命令，
  // 扣几件、加多少属性一律由后端算，客户端不报数值。战斗请求在途时先让路，
  // 免得把一次没发出的攻击顶掉。
  function commitBag(action, fields) {
    if (!data || data.role !== 'main' || pending || inFlight) return Promise.resolve(false);
    return commit(action, fields);
  }
  // The side documents are declared as functions of this factory, not only as methods of the
  // returned object: `load()` calls them by name, and an object literal's methods are *not*
  // in scope inside their siblings. Written as methods, each of these three calls was a
  // ReferenceError that the empty catches below swallowed, so a world that reopened showed an
  // empty layout (every placed block back in the bag) and no installed fragment.
  async function load() {
    const response = unwrapGameplay(await request({ action: 'load' }));
    if (response?.status !== 'ok') throw new Error(response?.message || '读取玩法进度失败');
    accept(response);
    loadFailures.length = 0;
    // A main world reports an empty layout, so one await covers both worlds.
    try { await loadPlacements(); } catch (error) {
      placementCache = null; loadFailures.push(error.message);
    }
    try { await loadWorldRules(); } catch (error) {
      ruleCache = null; loadFailures.push(error.message);
    }
    try { await loadSubworlds(); } catch (error) {
      rosterCache = null; loadFailures.push(error.message);
    }
    return data;
  }
  // Exhibits are authored one write at a time and never share the combat revision,
  // so a rejected layout can never corrupt a boss or its reward.
  async function loadPlacements() {
    const response = unwrapGameplay(await request({ action: 'loadPlacements' }));
    if (response?.status !== 'ok' || !response.state?.placements) {
      throw new Error(response?.message || '读取小世界陈列失败');
    }
    return (placementCache = response.state);
  }
  async function savePlacements(placed) {
    const response = unwrapGameplay(await request({ action: 'savePlacements', placements: placed }));
    if (response?.status !== 'ok' || !response.state?.placements) {
      throw new Error(response?.message || '保存小世界陈列失败');
    }
    // The scene must follow the confirmed layout, so mark the visuals dirty and let
    // the same reconcile path that handles combat redraw the exhibits.
    placementCache = response.state;
    visualDirty = true;
    return placementCache;
  }
  // World rules a small world was given by installing a fragment. They change no scene
  // membership, so unlike exhibits they never mark the visuals dirty: the rule engine
  // animates actors that already exist.
  async function loadWorldRules() {
    const response = unwrapGameplay(await request({ action: 'loadWorldRules' }));
    if (response?.status !== 'ok' || !Array.isArray(response.state?.rules)) {
      throw new Error(response?.message || '读取世界规则失败');
    }
    return (ruleCache = response.state);
  }
  async function saveWorldRules(rules) {
    const response = unwrapGameplay(await request({ action: 'saveWorldRules', rules }));
    if (response?.status !== 'ok' || !Array.isArray(response.state?.rules)) {
      throw new Error(response?.message || '保存世界规则失败');
    }
    return (ruleCache = response.state);
  }
  async function loadSubworlds() {
    const response = unwrapGameplay(await request({ action: 'loadSubworlds' }));
    if (response?.status !== 'ok' || !response.state?.subworlds) {
      throw new Error(response?.message || '读取小世界名册失败');
    }
    return (rosterCache = response.state);
  }
  // One entry at a time, like placements; the backend stays authoritative.
  async function addSubworld() {
    const response = unwrapGameplay(await request({ action: 'addSubworld' }));
    if (response?.status !== 'ok' || !response.state?.subworlds) {
      throw new Error(response?.message || '新增小世界失败');
    }
    rosterCache = response.state;
    return rosterCache;
  }
  async function renameSubworld(subworldId, name) {
    const response = unwrapGameplay(await request({ action: 'renameSubworld', subworldId, name }));
    if (response?.status !== 'ok' || !response.state?.subworlds) {
      throw new Error(response?.message || '重命名小世界失败');
    }
    rosterCache = response.state;
    return rosterCache;
  }
  // The boss and every living monster compete for the same swing. The page owns where
  // those actors are; this module only asks for their bounds and applies one nearest rule.
  function attackCandidates() {
    const player = readPlayer();
    const targets = [];
    const boss = readBoss();
    if (data.state.boss.hp > 0 && boss) {
      targets.push({ kind: 'boss', id: 'boss', name: 'Boss', bounds: worldBounds(boss) });
    }
    const monsters = readMonsters();
    for (const entry of Array.isArray(monsters) ? monsters : []) {
      if (!entry?.id || !entry.actor || !(data.state.monsters?.[entry.id]?.hp > 0)) continue;
      targets.push({ kind: 'monster', id: entry.id, name: entry.name,
        bounds: worldBounds(entry.actor) });
    }
    return { player, targets };
  }
  return {
    get data() { return data; },
    get busy() { return Boolean(inFlight); },
    get needsSave() { return Boolean(pending || visualDirty); },
    load,
    loadPlacements,
    savePlacements,
    get placements() { return placementCache; },
    loadWorldRules,
    saveWorldRules,
    get worldRules() { return ruleCache; },
    loadSubworlds,
    addSubworld,
    renameSubworld,
    get subworlds() { return rosterCache; },
    // Why a side document could not be read; empty means every document was read back.
    get loadFailures() { return [...loadFailures]; },
    attack() {
      if (inFlight) return inFlight;
      if (pending || visualDirty) return flush();
      if (!data || data.role !== 'main' || !canAct(readPlayer())) return Promise.resolve(false);
      const time = now();
      if (time - lastAttack < data.config.cooldownMs) return Promise.resolve(false);
      const { player, targets } = attackCandidates();
      if (!targets.length) return Promise.resolve(false);
      const target = nearestAttackTarget(player, targets, data.config);
      lastAttack = time;
      if (!target) { onFeedback('靠近并面向目标后攻击'); return Promise.resolve(false); }
      return commit(target.kind === 'boss' ? 'hitBoss' : 'hitMonster',
        target.kind === 'monster' ? { monsterId: target.id } : {});
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
      const { player, targets } = attackCandidates();
      const target = nearestAttackTarget(player, targets,
        { meleeRange: skill.range, meleeHalfAngle: skill.halfAngle });
      const hit = Boolean(target);
      lastAttack = time;
      skillUntil[skillId] = time + skill.cooldownMs;
      return commit('castSkill', { skillId, hit,
        ...(hit && target.kind === 'monster' ? { monsterId: target.id } : {}) });
    },
    flush,
    // 属性与技能数值由后端按已披挂的装备算好放在 config 里，页面直接显示即可。
    get attributes() { return data?.config?.attributes || null; },
    equipItem(itemId) { return commitBag('equipItem', { itemId }); },
    unequipItem(slot) { return commitBag('unequipItem', { slot }); },
    useItem(itemId) { return commitBag('useItem', { itemId }); },
    discardItem(itemId) { return commitBag('discardItem', { itemId }); },
  };
}
