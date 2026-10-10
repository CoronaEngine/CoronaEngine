import assert from 'node:assert/strict';
import test from 'node:test';
import { STORY_CHARACTERS, PLAYER_GUID, PLAYER_MODEL_REF, PROPHET_MODEL_REF, resolveStoryAssetPath, characterTransform, rotatedBounds } from '../../frontend/storyCharacters.mjs';
import { ensureStoryCharacters } from '../../frontend/storyActors.mjs';
import { actorFixture, apiFixture, url, deferred, projectReady } from './fixtures.mjs';
const load = (f, extra = {}) => ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url, wait: async () => {}, ...extra });

test('source/deployed/UNC asset resolution handles encoded Chinese and spaces', () => {
  assert.equal(resolveStoryAssetPath(url, STORY_CHARACTERS[0].asset), 'D:/Corona Engine/game/art/models/player/Maria WProp J J Ong.dae');
  assert.equal(resolveStoryAssetPath('file:///E:/游戏/CabbageEditor/Frontend/dist/index.html#/world', 'model/a #%.dae'), 'E:/游戏/game/art/models/model/a #%.dae');
  assert.equal(resolveStoryAssetPath('file://server/share/CabbageEditor/Frontend/dist/index.html', 'model/a.dae'), '//server/share/game/art/models/model/a.dae');
  assert.throws(() => resolveStoryAssetPath('http://localhost/index.html', 'a.dae'), /引擎文件页面/);
});
test('rotated normalized bounds determine scale and contact with y=0', () => {
  for (const character of STORY_CHARACTERS) {
    const actor = actorFixture(character), bounds = rotatedBounds(actor.local_aabb, character.rotation);
    const t = characterTransform(character, actor.local_aabb);
    assert.ok(Math.abs(t.position[1] + bounds[1] * t.scale[1]) < 1e-10);
    assert.ok(Math.abs((character.height ? bounds[4] - bounds[1] : Math.max(...[0,1,2].map(i => bounds[i+3]-bounds[i]))) * t.scale[0]
      - (character.height || character.size)) < 1e-10);
    assert.deepEqual([t.position[0], t.position[2]], [character.x, character.z]);
  }
  assert.throws(() => rotatedBounds([0,0,0,0,0,0], [0,0,0]), /包围盒/);
});
test('first load imports exactly four models; reopen is idempotent and keeps saved player pose', async () => {
  for (const wrapped of [false, true]) {
    const f = apiFixture({ wrapped });
    const first = await load(f);
    assert.equal(first.player.actor_guid, PLAYER_GUID);
    assert.equal(f.calls.filter(c => c[0] === 'create').length, 4);
    assert.ok(first.targetOffset > 0);
    f.get(PLAYER_GUID).geometry.position = [7, 0.9, -2];
    f.get(PLAYER_GUID).geometry.rotation = [0, 0.7, 0];
    const count = f.calls.length;
    const second = await load(f, { frontendUrl: 'http://invalid' });
    assert.equal(f.calls.length, count);
    assert.deepEqual(second.player.geometry.position, [7, 0.9, -2]);
    assert.deepEqual(second.player.geometry.rotation, [0, 0.7, 0]);
  }
});
test('unrelated names are untouched; fixed NPCs and disabled physics are enforced', async () => {
  const unrelated = { ...actorFixture(), actor_guid: 'someone-else' };
  const f = apiFixture({ actors: [unrelated, ...STORY_CHARACTERS.slice(1).map(c => actorFixture(c))] });
  const boss = f.get(STORY_CHARACTERS[1].guid);
  boss.geometry.position = [999,999,999]; boss.mechanics.physics_enabled = true;
  boss.follow_camera = true; boss.visible = false; boss.camera_lock.enabled = true;
  await load(f);
  assert.equal(f.state.actors.length, 5);
  assert.deepEqual(f.state.actors[0], unrelated);
  assert.equal(boss.mechanics.physics_enabled, false);
  assert.equal(boss.follow_camera, false); assert.equal(boss.camera_lock.enabled, false);
  assert.equal(boss.geometry.position[2], 12);
});
test('partial failure preserves successful actors; retry repairs incomplete placement without duplication', async () => {
  const f = apiFixture(); const original = f.api.scene.setActorTransform;
  f.api.scene.setActorTransform = async () => { throw new Error('disk failed'); };
  await assert.rejects(load(f), /玩家加载失败.*disk failed/);
  assert.equal(f.state.actors.length, 1);
  f.api.scene.setActorTransform = original;
  await load(f);
  assert.equal(f.state.actors.length, 4);
  assert.ok(f.get(PLAYER_GUID).geometry.position[1] > 0);
  assert.equal(f.calls.filter(c => c[0] === 'create').length, 4);
});
test('failed existing resources are rebound by GUID; failures identify role and source', async () => {
  const f = apiFixture({ actors: [actorFixture()] }); f.get(PLAYER_GUID).load_status = 'missing_resource';
  await load(f); assert.ok(f.calls.some(c => c[0] === 'rebind'));
  const broken = apiFixture(); broken.api.sceneTools.createActor = async () => { throw new Error('missing texture'); };
  await assert.rejects(load(broken), /玩家加载失败.*Maria.*missing texture/);
});
test('cancellation after an in-flight import never schedules another mutation', async () => {
  const f = apiFixture(), gate = deferred(); let current = true;
  const original = f.api.sceneTools.createActor;
  f.api.sceneTools.createActor = async (...args) => { const data = await original(...args); await gate.promise; return data; };
  const pending = load(f, { isCurrent: () => current });
  await new Promise(resolve => setImmediate(resolve)); current = false; gate.resolve();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(f.calls.length, 1);
});
test('GPU failure and timeout are not reported as successful initialization', async () => {
  const f = apiFixture({ actors: STORY_CHARACTERS.map(c => actorFixture(c)) });
  f.get(PLAYER_GUID).render_failed = true;
  await assert.rejects(load(f), /玩家加载失败.*模型渲染失败/);
  f.get(PLAYER_GUID).render_failed = false; f.get(PLAYER_GUID).render_ready = false; f.get(PLAYER_GUID).gpu_build_state = 'Pending';
  await assert.rejects(load(f, { renderAttempts: 2 }), /玩家.*超时/);
});

test('GPU Ready without valid render slots still waits for actual render readiness', async () => {
  const f = apiFixture({ actors: STORY_CHARACTERS.map(c => actorFixture(c)) });
  f.get(PLAYER_GUID).render_ready = false; f.get(PLAYER_GUID).gpu_build_state = 'Ready';
  await assert.rejects(load(f, { renderAttempts: 2 }), /玩家.*Maria.*超时/);
});


test('asynchronous imports wait for native bounds before scaling each character', async () => {
  for (const wrapped of [false, true]) {
    const f = apiFixture({ wrapped });
    const create = f.api.sceneTools.createActor;
    f.api.sceneTools.createActor = async (...args) => {
      await create(...args);
      const actor = f.get(args[3].actor_guid);
      actor.local_aabb = null;
      actor.render_ready = false;
      actor.gpu_build_state = 'PendingImport';
      return f.wrap({ status: 'success', actor: structuredClone(actor) });
    };
    let polls = 0;
    const result = await load(f, { wait: async () => {
      polls++;
      const actor = f.state.actors.at(-1);
      // No normalization/save may run while the importer still owns the bounds.
      assert.deepEqual(actor.geometry.scale, [1, 1, 1]);
      if (polls % 2) {
        actor.local_aabb = [0, 0, 0, 0, 0, 0];
        actor.gpu_build_state = 'PendingBuild';
      } else {
        const character = STORY_CHARACTERS.find(c => c.guid === actor.actor_guid);
        actor.local_aabb = actorFixture(character).local_aabb;
        actor.render_ready = true;
        actor.gpu_build_state = 'Ready';
      }
    } });
    assert.equal(polls, 8);
    assert.equal(f.calls.filter(c => c[0] === 'create').length, 4);
    assert.ok(result.targetOffset > 0);
    for (const character of STORY_CHARACTERS) {
      const actor = f.get(character.guid);
      assert.deepEqual(actor.geometry, characterTransform(character, actor.local_aabb));
    }
  }
});

test('unavailable bounds time out with the source path; retry reuses the original GUID', async () => {
  const f = apiFixture();
  const create = f.api.sceneTools.createActor;
  f.api.sceneTools.createActor = async (...args) => {
    await create(...args);
    f.get(args[3].actor_guid).local_aabb = null;
    return f.wrap({ status: 'success', actor: structuredClone(f.get(args[3].actor_guid)) });
  };
  let waits = 0;
  await assert.rejects(load(f, { renderAttempts: 2, wait: async () => { waits++; } }),
    /玩家加载失败.*game\/art\/models\/player\/Maria.*等待模型包围盒超时/);
  assert.equal(waits, 2);
  assert.equal(f.calls.length, 1);
  assert.equal(f.state.actors.length, 1);
  f.api.sceneTools.createActor = create;
  f.get(PLAYER_GUID).local_aabb = actorFixture().local_aabb;
  await load(f);
  assert.equal(f.calls.filter(c => c[0] === 'create').length, 4);
  assert.equal(f.state.actors.length, 4);
});

test('reopened actors also wait for bounds and retain the saved player transform', async () => {
  const saved = actorFixture();
  saved.geometry.position = [8, 0.9, -3];
  saved.geometry.rotation = [0, 0.6, 0];
  saved.local_aabb = null;
  const f = apiFixture({ actors: [saved, ...STORY_CHARACTERS.slice(1).map(c => actorFixture(c))] });
  const result = await load(f, { wait: async () => {
    f.get(PLAYER_GUID).local_aabb = actorFixture().local_aabb;
  } });
  assert.deepEqual(result.player.geometry, saved.geometry);
  assert.equal(f.calls.length, 0);
});

test('canceling while waiting for bounds prevents later reads, creates and transforms', async () => {
  const f = apiFixture({ actors: [actorFixture()] });
  f.get(PLAYER_GUID).local_aabb = null;
  let current = true, reads = 0;
  const snapshot = f.api.scene.getSnapshot;
  f.api.scene.getSnapshot = async (...args) => { reads++; return snapshot(...args); };
  await assert.rejects(load(f, { isCurrent: () => current, wait: async () => { current = false; } }),
    { name: 'AbortError' });
  assert.equal(reads, 1);
  assert.equal(f.calls.length, 0);
});

test('a failed async import reports the role without waiting for a bounds timeout', async () => {
  const f = apiFixture({ actors: [actorFixture()] });
  Object.assign(f.get(PLAYER_GUID), { local_aabb: null, render_failed: true, gpu_build_state: 'Failed' });
  let waited = false;
  await assert.rejects(load(f, { wait: async () => { waited = true; } }), /玩家加载失败.*模型导入失败/);
  assert.equal(waited, false);
  assert.equal(f.calls.length, 0);
});


test('legacy player facing is migrated with its marker in one write, preserving position and portable art', async () => {
  const player = actorFixture(); delete player.model_ref;
  player.route = 'Assets/Models/maria.dae';
  player.geometry.position = [7, 0.9, -9]; player.geometry.rotation = [0, 0.42, 0];
  const f = apiFixture({ actors: [player, ...STORY_CHARACTERS.slice(1).map(c => actorFixture(c))] });
  const result = await load(f, { frontendUrl: 'http://source-art-unavailable' });
  assert.deepEqual(result.player.geometry.position, player.geometry.position);
  assert.deepEqual(result.player.geometry.rotation, [0, 0.42 + Math.PI, 0]);
  assert.equal(result.player.model_ref, PLAYER_MODEL_REF);
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0][0], 'create');
  assert.equal(f.calls[0][2], player.route);
  assert.equal(f.calls[0][4].skip_if_exists, true); assert.equal(f.calls[0][4].update_if_exists, true);
  assert.equal(f.calls[0][4].model_ref, PLAYER_MODEL_REF);
  for (let i = 0; i < 3; i++) await load(f);
  assert.equal(f.calls.length, 1); assert.equal(f.state.actors.length, 4);
});
test('migration retries before/after commit never rotate twice or duplicate a player', async () => {
  for (const committed of [false, true]) {
    const player = actorFixture(); delete player.model_ref; player.geometry.rotation = [0, -0.8, 0];
    const f = apiFixture({ actors: [player] }), create = f.api.sceneTools.createActor;
    f.api.sceneTools.createActor = async (...args) => {
      if (committed) await create(...args);
      throw new Error(committed ? 'reply lost' : 'disk full');
    };
    await assert.rejects(load(f), /玩家加载失败.*(reply lost|disk full)/);
    f.api.sceneTools.createActor = create;
    await load(f); await load(f);
    assert.deepEqual(f.get(PLAYER_GUID).geometry.rotation, [0, -0.8 + Math.PI, 0]);
    assert.equal(f.get(PLAYER_GUID).model_ref, PLAYER_MODEL_REF); assert.equal(f.state.actors.length, 4);
  }
});
test('legacy incomplete import receives the default corrected pose, not a second half-turn', async () => {
  const player = actorFixture(); delete player.model_ref;
  player.geometry = { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] };
  const f = apiFixture({ actors: [player] }); await load(f); await load(f);
  assert.deepEqual(f.get(PLAYER_GUID).geometry.rotation, [0, Math.PI, 0]);
  assert.ok(f.get(PLAYER_GUID).geometry.position[1] > 0);
  assert.equal(f.state.actors.length, 4);
});
test('every versioned role is created with its placement marker in one call', async () => {
  // Regression: the prophet was created without its marker, so every entry took the
  // legacy repair path, which never persisted the marker. The world then failed to
  // load on every attempt with "引擎未确认玩家朝向版本".
  const f = apiFixture({ actors: [] });
  await load(f, { gameplay: { role: 'child', state: { boss: { hp: 0 }, drop: null } } });
  const creation = guid => f.calls.filter(call => call[0] === 'create' && call[1] === guid)
    .find(call => !call[4].skip_if_exists);
  assert.equal(creation(STORY_CHARACTERS[0].guid)[4].model_ref, PLAYER_MODEL_REF);
  assert.equal(creation(STORY_CHARACTERS[3].guid)[4].model_ref, PROPHET_MODEL_REF);
  // Committing the marker means the repair path is no longer needed to version the
  // actor. The prophet is the one exception: its first placement still has to be
  // normalized away from the raw unit-scale pose, and that must happen exactly once.
  const repairs = f.calls.filter(call => call[0] === 'create' && call[4].skip_if_exists);
  assert.deepEqual(repairs.map(call => call[1]), [STORY_CHARACTERS[3].guid]);
});

test('main hides NPCs; child reuses only prophet; corrected dragon bounds stay grounded', async () => {
  for (const role of ['main', 'child']) {
    const actors = STORY_CHARACTERS.map(c => actorFixture(c));
    actors[1].geometry.rotation = [Math.PI / 2, Math.PI, 0];
    actors[2].geometry.position = [-4, 0, 4]; actors[3].geometry.position = [4, 0, 4];
    const f = apiFixture({ actors });
    await load(f, { gameplay: { role, state: { boss: { hp: 200 }, drop: null } } });
    assert.deepEqual(f.get(STORY_CHARACTERS[2].guid).geometry.position.filter((_, i) => i !== 1), [-4, 4]);
    assert.deepEqual(f.get(STORY_CHARACTERS[3].guid).geometry.position.filter((_, i) => i !== 1), [4, 4]);
    assert.equal(f.calls.filter(call => call[0] === 'create').length, 0);
    assert.equal(f.state.actors.length, 4);
    assert.equal(f.get(STORY_CHARACTERS[2].guid).visible, false);
    assert.equal(f.get(STORY_CHARACTERS[3].guid).visible, role === 'child');
    const boss = f.get(STORY_CHARACTERS[1].guid);
    if (role === 'main') {
      assert.deepEqual(boss.geometry.rotation, [Math.PI / 2, 0, 0]);
      const bounds = rotatedBounds(boss.local_aabb, boss.geometry.rotation);
      assert.ok(Math.abs(bounds[1] * boss.geometry.scale[1] + boss.geometry.position[1]) < 1e-9);
    } else assert.equal(boss.visible, false);
  }
});


test('each character reaches real render readiness before the next character is submitted', async () => {
  const f = apiFixture(), gate = deferred(), entered = deferred();
  const create = f.api.sceneTools.createActor;
  f.api.sceneTools.createActor = async (...args) => {
    const result = await create(...args);
    if (args[3].actor_guid === PLAYER_GUID) {
      f.get(PLAYER_GUID).render_ready = false;
      (result.data ?? result).actor.render_ready = false;
    }
    return result;
  };
  const loading = load(f, { wait: async ms => { assert.equal(ms, 200); entered.resolve(); await gate.promise; } });
  await entered.promise;
  assert.deepEqual(f.calls.filter(c => c[0] === 'create').map(c => c[1]), [PLAYER_GUID]);
  f.get(PLAYER_GUID).render_ready = true;
  gate.resolve();
  await loading;
  assert.deepEqual(f.calls.filter(c => c[0] === 'create').map(c => c[1]), STORY_CHARACTERS.map(c => c.guid));
});

test('gameplay visibility leaves unrelated scene models untouched', async () => {
  const scenery = [undefined, 'model', 'actor', 'audio', 'ui'].map((actor_type, index) => ({
    ...actorFixture(), actor_guid: `scenery-${index}`, actor_type,
  }));
  scenery.push({ ...actorFixture(), actor_guid: 'hidden-scenery', visible: false });
  for (const role of ['main', 'child']) for (const combatOnly of [false, true]) {
    const f = apiFixture({ actors: [...scenery, ...STORY_CHARACTERS.map(c => actorFixture(c))] });
    await load(f, { gameplay: { role, state: { boss: { hp: 200 }, drop: null } }, combatOnly });
    for (const actor of scenery) assert.deepEqual(f.get(actor.actor_guid), actor);
    assert.ok(f.calls.every(call => !scenery.some(actor => actor.actor_guid === call[1])));
    assert.equal(f.get(STORY_CHARACTERS[2].guid).visible, false);
    assert.equal(f.get(STORY_CHARACTERS[1].guid).visible, role === 'main');
    assert.equal(f.get(STORY_CHARACTERS[3].guid).visible, role === 'child');
  }
});

test('interrupted prophet creation is grounded on retry and then preserved', async () => {
  const player = actorFixture(), prophet = STORY_CHARACTERS[3];
  player.geometry.position = [7, 0.9, -3];
  player.geometry.rotation = [0, Math.PI + 0.4, 0];
  const f = apiFixture({ actors: [player] });
  const gameplay = { role: 'child', state: { boss: { hp: 0 }, drop: null } };
  const create = f.api.sceneTools.createActor;
  let current = true;
  f.api.sceneTools.createActor = async (...args) => {
    const result = await create(...args);
    if (args[3].actor_guid === prophet.guid && !args[3].skip_if_exists) current = false;
    return result;
  };
  await assert.rejects(load(f, { gameplay, isCurrent: () => current }), { name: 'AbortError' });
  // Creation owns the placement marker immediately, so a later entry never mistakes
  // this actor for an unversioned legacy one.
  assert.equal(f.get(prophet.guid).model_ref, PROPHET_MODEL_REF);

  current = true;
  await load(f, { gameplay, frontendUrl: 'http://source-art-unavailable' });
  // The raw unit-scale placeholder pose must be normalized and grounded in this same
  // load: the prophet is placed on demand, so a second entry never comes to fix it.
  const expected = characterTransform(prophet, f.get(prophet.guid).local_aabb);
  assert.deepEqual(f.get(prophet.guid).geometry, expected);
  assert.deepEqual(expected.position.slice(0, 3), [prophet.x, 0.9, prophet.z]);
  assert.deepEqual(expected.rotation, [...prophet.rotation]);
  assert.equal(f.get(prophet.guid).model_ref, PROPHET_MODEL_REF);
  assert.equal(f.state.actors.length, 2);

  // Once the committed pose matches, re-entering is a no-op that preserves it.
  const writes = f.calls.length;
  await load(f, { gameplay, frontendUrl: 'http://source-art-unavailable' });
  assert.equal(f.calls.length, writes);
  assert.deepEqual(f.get(prophet.guid).geometry, expected);
});


test('story initialization waits for project resources, validates their source and drains failures', async () => {
  const f = apiFixture(), waits = []; let reads = 0;
  f.api.project = { getProjectLoadStatus: async () => ({ data: {
    ...projectReady('d:/story'), pending: ++reads === 1 ? 1 : 0,
  } }) };
  await load(f, { projectPath: 'D:\\story\\', wait: async ms => waits.push(ms) });
  assert.deepEqual(waits, [200]);
  f.api.project.getProjectLoadStatus = async () => projectReady('D:/other');
  await assert.rejects(load(f, { projectPath: 'D:/story' }), /不属于目标世界/);
  const gate = deferred(), entered = deferred(); reads = 0;
  f.api.project.getProjectLoadStatus = async () => ({ ...projectReady('D:/story'), failed: 1, pending: ++reads === 1 ? 1 : 0 });
  let settled = false;
  const outcome = assert.rejects(load(f, { projectPath: 'D:/story', wait: async () => {
    entered.resolve(); await gate.promise;
  } }), /1 个模型/).then(() => { settled = true; });
  await entered.promise; assert.equal(settled, false);
  gate.resolve(); await outcome;
});

test('canceling an import keeps its resource work pending without submitting more mutations', async () => {
  const f = apiFixture(), gate = deferred(), entered = deferred();
  let current = true, pending = 0, settled = false;
  f.api.project = { getProjectLoadStatus: async () => ({ ...projectReady('D:/story'), pending }) };
  const create = f.api.sceneTools.createActor;
  f.api.sceneTools.createActor = async (...args) => {
    const result = await create(...args); current = false; pending = 1; return result;
  };
  const outcome = assert.rejects(load(f, { projectPath: 'D:/story', isCurrent: () => current, wait: async () => {
    entered.resolve(); await gate.promise; pending = 0;
  } }), { name: 'AbortError' }).then(() => { settled = true; });
  await entered.promise; assert.equal(settled, false);
  assert.deepEqual(f.calls.map(c => c[0]), ['create']);
  gate.resolve(); await outcome;
  assert.deepEqual(f.calls.map(c => c[0]), ['create']);
});
