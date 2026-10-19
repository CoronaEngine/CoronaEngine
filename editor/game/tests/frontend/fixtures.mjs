import { STORY_CHARACTERS, BOSS_MODEL_REF, PLAYER_MODEL_REF, PROPHET_MODEL_REF, characterTransform } from '../../frontend/storyCharacters.mjs';
export const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
export const url = 'file:///D:/Corona%20Engine/editor/Frontend/dist/index.html#/world';
/** Characters the fake engine can materialise, keyed by guid. The story table carries the
 * four inhabitants and the boss, so this is a straight projection of it; kept exported so a
 * test that adds a view module can reuse one character definition. */
export const CHARACTER_BY_GUID = new Map(STORY_CHARACTERS.map(character => [character.guid, character]));

export function actorFixture(character = STORY_CHARACTERS[0]) {
  const local_aabb = character.role === 'boss' ? [-0.5, -0.325, -0.107, 0.5, 0.325, 0.107]
    : [-0.5, -0.4, -0.25, 0.5, 0.4, 0.25];
  // The boss ships at the current generation, so a fixture world is "already in place" and the
  // ordinary entry path touches nothing. A test that wants the stale-dragon migration deletes
  // this marker explicitly, which is what `a legacy boss is replaced...` does.
  const marker = character.role === 'player' ? PLAYER_MODEL_REF
    : character.role === 'prophet' ? PROPHET_MODEL_REF
      : character.role === 'boss' ? BOSS_MODEL_REF : null;
  // A stable positive handle derived from the guid, never the table index: a character a
  // test builds by hand is not in the table (`indexOf` answers -1), and `loaded()` in
  // storyActors.mjs only trusts a positive handle.
  const handle = 100 + [...character.guid].reduce((sum, ch) => sum + ch.charCodeAt(0), 0) % 900;
  return { ...(marker ? { model_ref: marker } : {}), name: character.name, actor_guid: character.guid, handle,
    load_status: 'loaded', render_ready: true, gpu_build_state: 'Ready',
    local_aabb, geometry: characterTransform(character, local_aabb),
    mechanics: { physics_enabled: false }, visible: true, follow_camera: false, camera_lock: { enabled: false } };
}
export function sceneFixture(actors = STORY_CHARACTERS.map(c => actorFixture(c))) {
  return { scene: 'scene.ini', actors, active_camera_name: 'main', cameras: [
    { handle: 12, name: 'main', position: [0, 0, -5], forward: [0, 0, 1], world_up: [0, 1, 0], fov: 60 },
  ] };
}
export function apiFixture({ actors = [], wrapped = true } = {}) {
  const state = sceneFixture(structuredClone(actors)), calls = [];
  const wrap = data => wrapped ? { data } : data;
  const get = guid => state.actors.find(a => a.actor_guid === guid);
  const mutate = (op, guid, fn) => { calls.push([op, guid]); const actor = get(guid); fn(actor); return wrap({ status: 'success', actor: structuredClone(actor) }); };
  const api = {
    main: { onInit: async () => wrap({ scenes: [{ path: 'scene.ini' }] }) },
    scene: {
      getSnapshot: async () => wrap(structuredClone(state)),
      setActorTransform: async (scene, guid, transform) => mutate('transform', guid, a => {
        for (const key of ['position', 'rotation', 'scale']) if (transform[key]) a.geometry[key] = [...transform[key]];
      }),
    },
    sceneTools: {
      createActor: async (scene, path, type, data) => {
        calls.push(['create', data.actor_guid, path, type, data]);
        if (data.skip_if_exists && get(data.actor_guid)) {
          const actor = get(data.actor_guid);
          // Mirrors the engine's repair branch (create_actor, skip_if_exists + update_if_exists,
          // cef_editor_native_api_handlers.cpp:3410-3424): it rewrites the transform and the
          // runtime metadata and NOTHING else — in particular it never rewrites `actor.route`,
          // which is why a stale asset generation cannot be repaired in place.
          if (data.update_if_exists) {
            for (const key of ['position', 'rotation', 'scale']) if (data[key]) actor.geometry[key] = [...data[key]];
            if (data.model_ref) actor.model_ref = data.model_ref;
            if (data.body_type) actor.mechanics = { ...actor.mechanics, body_type: data.body_type };
          }
          return wrap({ status: 'success', actor: structuredClone(actor), existed: true });
        }
        const character = CHARACTER_BY_GUID.get(data.actor_guid);
        const actor = { ...actorFixture(character), model_ref: data.model_ref || '', route: `Assets/${path.split('/').at(-1)}`,
          geometry: { position: [...data.position], rotation: [...data.rotation], scale: [...data.scale] } };
        state.actors.push(actor);
        return wrap({ status: 'success', actor: structuredClone(actor) });
      },
      removeActor: async (scene, guid) => {
        calls.push(['remove', guid]);
        const index = state.actors.findIndex(actor => actor.actor_guid === guid || actor.name === guid);
        if (index < 0) return wrap({ status: 'error', message: `Actor not found: ${guid}` });
        state.actors.splice(index, 1);
        return wrap({ status: 'success', actor_guid: guid });
      },
      rebindActorResource: async (scene, guid, path) => mutate('rebind', guid, a => {
        a.load_status = 'loaded'; a.handle = 101; a.route = path;
      }),
      setActorPhysics: async (scene, guid, physics) => mutate('physics', guid, a => { Object.assign(a.mechanics, physics); }),
      setActorState: async (scene, guid, data) => mutate('state', guid, a => Object.assign(a, data)),
      setActorCameraLock: async (scene, guid, data) => mutate('lock', guid, a => { Object.assign(a.camera_lock, data); }),
    },
  };
  return { api, calls, state, get, wrap };
}

export const gameplayConfig = { playerHp: 100, rageMax: 100, ragePerHit: 10, bossHp: 200,
  damage: 20, cooldownMs: 400, bossBarRadius: 10, monsterBarRadius: 10,
  meleeRange: 2.5, meleeHalfAngle: Math.PI / 3,
  monsterDropMin: 5, monsterDropMax: 15, monsterEquipmentChance: 0.5,
  monsters: {
    'monster-em135': { hp: 180 },
    'monster-em001-a': { hp: 120 },
    'monster-em001-b': { hp: 120 },
    'monster-em001-c': { hp: 120 },
    'monster-em3': { hp: 160 },
    'monster-em1': { hp: 80 },
    'monster-em2': { hp: 90 },
  },
  skills: {
    heavy: { key: 'E', name: '重斩', damage: 50, rageCost: 30, range: 2.5, halfAngle: Math.PI / 3, cooldownMs: 1200 },
    sweep: { key: 'R', name: '横扫', damage: 80, rageCost: 50, range: 3.5, halfAngle: Math.PI / 2, cooldownMs: 3000 },
  },
};
export const projectReady = path => ({ active: true, archive_service_ready: true, path,
  pending: 0, failed: 0, ready: 2, total: 2, loading: false });
