import assert from 'node:assert/strict';
import test from 'node:test';
import { HOTBAR_SLOTS, MAX_PLACEMENTS, PLACED_BLOCK_REACH, PLACEMENT_PREFIX, PLACEMENT_SITES,
  blockSpawnTransform, canPlace, canonicalPropId, cycleSlot, exhibitItems, findProp, hotbarSlots,
  nearestPlacedBlock, nextPlacementIndex, placeableProps, placementGuid, placementIndex,
  removePlacement, sanitizePlacements, validPlacement } from '../../frontend/storyProps.mjs';

const FORWARD_DISTANCE = 2.0;
const close = (actual, expected, message) =>
  assert.ok(Math.abs(actual - expected) < 1e-9, `${message}：期望 ${expected}，实际 ${actual}`);

test('the block lands on the ground directly in front of the player, for any facing', () => {
  const player = { position: [3, 1.25, -4] };
  for (const yaw of [0, Math.PI / 6, Math.PI / 2, Math.PI, -Math.PI / 2, 2.5, -7]) {
    const spawn = blockSpawnTransform(player, yaw, FORWARD_DISTANCE);
    // The same (sin, cos) forward convention storyCube.screenAxes uses.
    close(spawn.position[0], player.position[0] + Math.sin(yaw) * FORWARD_DISTANCE, `yaw ${yaw} · x`);
    close(spawn.position[2], player.position[2] + Math.cos(yaw) * FORWARD_DISTANCE, `yaw ${yaw} · z`);
    close(spawn.position[1], player.position[1], `yaw ${yaw} · 站在玩家脚下的地面`);
    assert.deepEqual(spawn.rotation, [0, yaw, 0]);
    assert.equal(spawn.scale, 1);
    // A player-authored spot is never a site.
    assert.equal(spawn.siteId, null);
  }
  close(blockSpawnTransform(player, 0).position[2], player.position[2] + FORWARD_DISTANCE,
    '默认距离在前方');
});

test('a spawn that cannot be trusted is refused instead of guessed', () => {
  for (const bad of [null, {}, { position: [0, 0] }, { position: [0, 0, NaN] },
    { position: [0, 0, '0'] }, { position: [0, 0, Infinity] }]) {
    assert.throws(() => blockSpawnTransform(bad, 0), /玩家位置未就绪/);
  }
  assert.throws(() => blockSpawnTransform({ position: [0, 0, 0] }, NaN), /玩家朝向未就绪/);
  assert.throws(() => blockSpawnTransform({ position: [0, 0, 0] }, 0, 0), /摆放距离无效/);
});

// 物体必须站在地面上：地面线由烘好的高度网格回答（与角色同一个来源），
// 读不到网格时才退回摆放前的常量（玩家自己的 y）。
test('the block lands on the ground grid at its own spot, and on the old constant without one', () => {
  const player = { position: [0, 2.6, 0] };
  const ground = (x, z) => 1 + 0.5 * x + 0.25 * z;
  const onGrid = blockSpawnTransform(player, 0, FORWARD_DISTANCE, ground);
  close(onGrid.position[0], 0, 'x 不变');
  close(onGrid.position[2], FORWARD_DISTANCE, 'z 不变');
  // 落点是方块自己那一格的地面，不是玩家脚下的高度（1.9 是玩家原点的世界 y）。
  close(onGrid.position[1], ground(onGrid.position[0], onGrid.position[2]),
    '有网格时 y 必须是落点自己的地面高度');
  assert.ok(Math.abs(onGrid.position[1] - player.position[1]) > 0.5,
    '与旧常量必须差得可观测，否则这条断言是空转的');

  // 玩家站在上坡上、前方更高：仍然取前方那一点的地面，而不是把玩家抬起来。
  const uphill = blockSpawnTransform({ position: [0, 5, 0] }, 0, FORWARD_DISTANCE, () => 2);
  close(uphill.position[1], 2, '地面比玩家低也必须按地面来');

  // 没有网格（null / 未给 / 不是函数 / 采样不是有限数）时，就是旧行为。
  for (const none of [undefined, null, 0, [], () => null, () => NaN, () => '1']) {
    close(blockSpawnTransform(player, 0, FORWARD_DISTANCE, none).position[1], player.position[1],
      `没有可用地面时必须退回旧常量（${String(none)}）`);
  }
});

test('what a spawn produces is exactly what the placement layer accepts', () => {
  const spawn = blockSpawnTransform({ position: [1, 0, 2] }, Math.PI / 4);
  const entry = { propId: findProp('block').id, index: 0, ...spawn };
  assert.equal(validPlacement(entry), true, '自由摆放（siteId: null）必须合法');
  assert.deepEqual(sanitizePlacements([entry]), [entry], '且原样通过净化');
  assert.deepEqual(PLACEMENT_SITES.map(site => site.id), ['west', 'east', 'north', 'south'],
    '点位表仍在导出，但页面不再使用它');
});

// The guards in StoryWorld.vue read these three pure rules; this keeps them honest.
const atCapacity = placements => nextPlacementIndex(placements) >= MAX_PLACEMENTS;
const placementAt = index => ({ propId: 'block', index, siteId: null,
  position: [index % 8, 0, Math.floor(index / 8)], rotation: [0, 0, 0], scale: 1 });
const full = Array.from({ length: MAX_PLACEMENTS }, (unused, index) => placementAt(index));

test('the free count is always owned minus placed, per item', () => {
  const inventory = { worldFragment: 2, block: 64 };
  const placed = [
    placementAt(1), placementAt(3),
    { ...placementAt(5), propId: 'world-fragment' },
  ];
  const items = exhibitItems(inventory, placed);
  // 前两件是搭建件，其余是这次新加的植被（没库存就全是 0）。
  const rows = items.map(item => [item.id, item.owned, item.placed, item.available]);
  assert.deepEqual(rows.slice(0, 2), [['block', 64, 2, 62], ['ball', 0, 0, 0]]);
  assert.ok(rows.slice(2).every(row => row[1] === 0 && row[2] === 0 && row[3] === 0));
  // Every placed block is one block fewer in the bag.
  assert.deepEqual(exhibitItems(inventory, [...placed, placementAt(7)])
    .map(item => [item.id, item.available]).slice(0, 2), [['block', 61], ['ball', 0]]);
  // Never negative, never more than the bag holds.
  assert.ok(exhibitItems({}, placed).every(item => item.available === 0));
  assert.ok(exhibitItems({ block: 1 }, [placementAt(0), placementAt(1), placementAt(2)])
    .every(item => item.available === 0));
  assert.equal(findProp('block').asset, 'cube/cube.obj');
  assert.equal(findProp('ball').asset, 'fragment/Ball.obj');
  // 退役的石柱 id 指向球：旧布局与旧存档不会丢东西。
  assert.equal(canonicalPropId('pillar'), 'ball');
  assert.equal(findProp('pillar').id, 'ball');
  assert.equal(canPlace('pillar'), true);
});

test('the world fragment can never be placed, so it is never an item bar entry', () => {
  assert.equal(findProp('world-fragment').placeable, false);
  assert.equal(findProp('block').placeable, true);
  assert.deepEqual(placeableProps().map(prop => prop.id), ['block', 'ball', 'tree1', 'tree2', 'tree3', 'grass1', 'grass2', 'grass4', 'grass5']);
  assert.equal(canPlace('block'), true);
  assert.equal(canPlace('ball'), true);
  assert.equal(canPlace('world-fragment'), false);
  assert.equal(canPlace('no-such-prop'), false, '未知 id 不得抛错，也不得放行');
  // Even holding fragments changes nothing: they simply do not appear as placeable.
  const items = exhibitItems({ worldFragment: 9, block: 3 }, []);
  assert.deepEqual(items.map(item => item.id), ['block', 'ball', 'tree1', 'tree2', 'tree3', 'grass1', 'grass2', 'grass4', 'grass5']);
  assert.deepEqual(items.map(item => item.owned), [3, 0, 0, 0, 0, 0, 0, 0, 0]);
  // A fragment entry already in a save stays valid (it is not destroyed), but it is still
  // not offered as something new to put down.
  const stranded = [{ ...placementAt(0), propId: 'world-fragment' }];
  assert.deepEqual(sanitizePlacements(stranded).map(entry => entry.propId), ['world-fragment']);
  assert.deepEqual(exhibitItems({ worldFragment: 9 }, stranded).map(item => item.id),
    ['block', 'ball', 'tree1', 'tree2', 'tree3', 'grass1', 'grass2', 'grass4', 'grass5']);
  // 旧布局里的石柱在净化时直接变成球，不会在进入小世界时被丢掉。
  const legacyPillar = [{ ...placementAt(0), propId: 'pillar' }];
  assert.deepEqual(sanitizePlacements(legacyPillar).map(entry => entry.propId), ['ball']);
});

test('a full world refuses the next placement instead of losing it on save', () => {
  assert.equal(MAX_PLACEMENTS, 64, '后端 placements.MAX_PLACEMENTS 的镜像');
  assert.equal(atCapacity(full), true, '64 件之后不再接受新的摆放');
  assert.equal(atCapacity(full.slice(0, MAX_PLACEMENTS - 1)), false);
  assert.equal(atCapacity([]), false);
  // The lowest free index is what the page writes; at capacity there is none left, and the
  // guard fires before the save — the backend rejects `index >= MAX_PLACEMENTS` outright, so
  // a placement made past the ceiling would cost the player a block and save nothing.
  assert.equal(nextPlacementIndex(full.slice(0, MAX_PLACEMENTS - 1)), MAX_PLACEMENTS - 1);
  assert.equal(nextPlacementIndex(full), MAX_PLACEMENTS);
  assert.equal(atCapacity(full.slice(0, 30)), false, '差一件时仍可摆放');
  // A removed entry frees its index again, so the count can go back down.
  const gap = [...full.slice(0, 10), ...full.slice(11)];
  assert.equal(nextPlacementIndex(gap), 10);
  assert.equal(atCapacity(gap), false);
});

// The wheel steps the item bar one slot at a time; StoryWorld.vue routes the surface wheel
// through this rule and adds no wrap logic of its own.
// 物品栏是固定的一排格子（Minecraft 式）：有物的占前几格，其余是空格子。StoryWorld.vue
// 直接渲染这个列表，滚轮与数字键都在它的长度上换格，因此长度必须是常数 HOTBAR_SLOTS。
const emptySlot = slot => ({ slot, id: null, name: '', owned: 0, placed: 0, available: 0, empty: true });

test('the item bar always has 9 slots, empties included', () => {
  assert.equal(HOTBAR_SLOTS, 9);
  const slots = hotbarSlots({ block: 64 }, []);
  assert.equal(slots.length, HOTBAR_SLOTS);
  // 第一格是方块本身，可用数量照旧来自 exhibitItems。
  assert.deepEqual(slots[0], { slot: 0, id: 'block', name: '方块',
    description: findProp('block').description, owned: 64, placed: 0, available: 64, empty: false });
  // 其后全是空格子，形状固定，编号与下标一致。
  assert.deepEqual(slots.slice(1), [1, 2, 3, 4, 5, 6, 7, 8].map(emptySlot));
  // 空手也是一整排空格子，而不是零格：否则滚轮与数字键就没有格子可选。
  assert.deepEqual(hotbarSlots({}, []), Array.from({ length: HOTBAR_SLOTS }, (unused, slot) => emptySlot(slot)));
  assert.deepEqual(hotbarSlots({ worldFragment: 9 }, []), hotbarSlots({}, []),
    '世界碎片既不可摆放，也不进物品栏');
  // 已摆放的方块仍然计入可用数量，格子本身保持非空。
  const placed = hotbarSlots({ block: 3 }, [placementAt(0), placementAt(1)]);
  assert.equal(placed[0].available, 1);
  assert.equal(placed[0].empty, false);
  // 名下的东西全部摆完时格子仍是「方块 · 0」，页面提示「已用完」而不是「这一格是空的」。
  const spent = hotbarSlots({ block: 1 }, [placementAt(0)]);
  assert.equal(spent[0].id, 'block');
  assert.equal(spent[0].owned, 1);
  assert.equal(spent[0].available, 0);
  assert.equal(spent[0].empty, false);
});

test('a bad bar size falls back instead of breaking the row', () => {
  for (const bad of [0, -1, -8, NaN, 1.5, Infinity, '8', null, undefined]) {
    const slots = hotbarSlots({ block: 64 }, [], bad);
    assert.equal(slots.length, HOTBAR_SLOTS, `尺寸 ${String(bad)} 必须退回 ${HOTBAR_SLOTS}`);
    assert.equal(slots[0].id, 'block');
  }
  // 合法的小尺寸仍然被尊重：函数是按尺寸切格，不是只会数到 8。
  assert.deepEqual(hotbarSlots({ block: 64 }, [], 2).map(entry => entry.id), ['block', null]);
});

test('the wheel wraps the item bar selection in both directions', () => {
  const LENGTH = 3;
  // One wheel event is one slot, whichever way the wheel turned.
  assert.equal(cycleSlot(0, LENGTH, 1), 1);
  assert.equal(cycleSlot(2, LENGTH, -1), 1);
  // Wrapping: forward off the end returns to the start, backward off the start goes to the end.
  assert.equal(cycleSlot(LENGTH - 1, LENGTH, 1), 0, '向后滚过末格回到首格');
  assert.equal(cycleSlot(0, LENGTH, -1), LENGTH - 1, '向前滚过首格回到末格');
  // A real wheel delta is never ±1, so only its sign may drive the bar.
  assert.equal(cycleSlot(0, LENGTH, -400), LENGTH - 1);
  assert.equal(cycleSlot(LENGTH - 1, LENGTH, 120), 0);
  assert.equal(cycleSlot(1, LENGTH, 0), 1, '零位移不换格');
  // An item bar with nothing to choose between has no selection to move.
  assert.equal(cycleSlot(0, 0, 1), 0);
  assert.equal(cycleSlot(4, 0, -1), 0);
  assert.equal(cycleSlot(3, 1, 1), 0);
  assert.equal(cycleSlot(0, 1, -999), 0);
  assert.equal(cycleSlot(2, -5, 1), 0, '长度不合法时不得抛错');
  // A large delta keeps wrapping instead of running off the row.
  assert.equal(cycleSlot(0, LENGTH, 10_000), 1);
  assert.equal(cycleSlot(1, LENGTH, -10_000), 0);
  for (let step = -12; step <= 12; step++) {
    for (let current = 0; current < LENGTH; current++) {
      const next = cycleSlot(current, LENGTH, step);
      assert.ok(Number.isInteger(next) && next >= 0 && next < LENGTH,
        `步进 ${step}（当前 ${current}）必须落在 0..${LENGTH - 1}`);
    }
  }
  // 真正的物品栏是 8 格：空格子同样在环绕之内，因此从第 8 格向前会回到第 1 格。
  assert.equal(cycleSlot(HOTBAR_SLOTS - 1, HOTBAR_SLOTS, 1), 0, '从第 8 格向后滚到第 1 格');
  assert.equal(cycleSlot(0, HOTBAR_SLOTS, -1), HOTBAR_SLOTS - 1, '从第 1 格向前滚到第 8 格');
  assert.equal(cycleSlot(HOTBAR_SLOTS - 1, HOTBAR_SLOTS, 120), 0);
  assert.equal(cycleSlot(0, HOTBAR_SLOTS, -400), HOTBAR_SLOTS - 1);
  for (let step = -12; step <= 12; step++) {
    for (let current = 0; current < HOTBAR_SLOTS; current++) {
      const next = cycleSlot(current, HOTBAR_SLOTS, step);
      assert.ok(Number.isInteger(next) && next >= 0 && next < HOTBAR_SLOTS,
        `步进 ${step}（当前 ${current}）必须落在 0..${HOTBAR_SLOTS - 1}`);
    }
  }
});

// 取回：F 的候选由这份纯几何决定，物品栏数字由 removePlacement 决定。两者都不能碰存档形状。
test('the nearest placed block inside the reach is the one F can take back', () => {
  const block = (index, x, z = 0) => ({ propId: 'block', index, siteId: null,
    position: [x, 0, z], rotation: [0, 0, 0], scale: 1 });
  const player = { position: [0, 1.6, 0] };

  // 1 米外的一件：在射程内，距离按 x/z 量（y 不参与，红球与陈列物用的是同一把尺子）。
  assert.deepEqual(nearestPlacedBlock(player, [block(0, 1)], PLACED_BLOCK_REACH),
    { index: 0, propId: 'block', distance: 1 });
  // 边界含端点：恰好等于射程算命中，超出一点点就不算。
  assert.equal(nearestPlacedBlock(player, [block(3, PLACED_BLOCK_REACH)], PLACED_BLOCK_REACH).index, 3);
  assert.equal(nearestPlacedBlock(player, [block(3, PLACED_BLOCK_REACH + 0.01)], PLACED_BLOCK_REACH), null);
  // 摆放的前方是 2 米，因此取回的射程必须覆盖它：放下之后原地按 F 就能拿回来。
  assert.deepEqual(nearestPlacedBlock(player, [block(0, 0, 2.0)], PLACED_BLOCK_REACH),
    { index: 0, propId: 'block', distance: 2 });
  // 多件：最近的那件胜，并且带的是它自己的序号（序号就是场景里的 actor 身份）。
  const many = [block(4, 2.4), block(9, -0.5, 0.1), block(1, 1.2)];
  assert.equal(nearestPlacedBlock(player, many, PLACED_BLOCK_REACH).index, 9);
  assert.equal(nearestPlacedBlock(player, [block(4, 2.4), block(1, 1.2)], PLACED_BLOCK_REACH).index, 1);
  // 抬高的方块仍按水平距离算：站在它下面也算“就在身边”。
  const raised = [{ ...block(0, 0), position: [0, 9, 0] }];
  assert.equal(nearestPlacedBlock(player, raised, PLACED_BLOCK_REACH).distance, 0);
  assert.equal(nearestPlacedBlock(player, raised, PLACED_BLOCK_REACH).index, 0);
  // 空布局 / 脏输入：一律回答“没有”，绝不抛错。
  assert.equal(nearestPlacedBlock(player, [], PLACED_BLOCK_REACH), null);
  for (const bad of [null, undefined, 42, 'blocks', {}, [[]], [{ propId: 'block' }]]) {
    assert.equal(nearestPlacedBlock(player, bad, PLACED_BLOCK_REACH), null, `${String(bad)} 不得成为候选`);
  }
  for (const bad of [null, {}, { position: [0, 0] }, { position: [0, NaN, 0] },
    { position: [0, 0, '0'] }, { position: [0, 0, Infinity] }, { position: [0, 0, 0, 0] }]) {
    assert.equal(nearestPlacedBlock(bad, [block(0, 1)], PLACED_BLOCK_REACH), null, '玩家位置不合法就没有候选');
  }
  for (const bad of [NaN, -1, -0.5, Infinity, '2', null]) {
    assert.equal(nearestPlacedBlock(player, [block(0, 1)], bad), null, `射程 ${String(bad)} 不得放行`);
  }
  // 省略射程＝命名常量（与传 undefined 同义），而不是无限。
  assert.equal(nearestPlacedBlock(player, [block(0, 1)], undefined).distance, 1);
  assert.equal(nearestPlacedBlock(player, [block(0, PLACED_BLOCK_REACH + 0.01)], undefined), null);
  // 默认射程就是那条命名常量：调用方忘了传，也不会悄悄变成无限。
  assert.equal(nearestPlacedBlock(player, [block(0, PLACED_BLOCK_REACH + 1)]), null);
  assert.equal(nearestPlacedBlock(player, [block(0, 1)]).distance, 1);
});

test('a keepsake in the layout is never something F can take back', () => {
  const player = { position: [0, 0, 0] };
  const entry = (propId, index, x) => ({ propId, index, siteId: null,
    position: [x, 0, 0], rotation: [0, 0, 0], scale: 1 });
  // 世界碎片不可摆放，因此即使它就在脚下，也不是候选。
  assert.equal(canPlace('world-fragment'), false);
  assert.equal(nearestPlacedBlock(player, [entry('world-fragment', 0, 0)], PLACED_BLOCK_REACH), null);
  assert.equal(nearestPlacedBlock(player, [entry('no-such-prop', 1, 0)], PLACED_BLOCK_REACH), null);
  // 混合时只有可摆放的那件参与比较，近在咫尺的碎片不能把它挡掉。
  assert.deepEqual(nearestPlacedBlock(player,
    [entry('world-fragment', 0, 0.1), entry('block', 5, 1)], PLACED_BLOCK_REACH),
  { index: 5, propId: 'block', distance: 1 });
});

test('removing one placement leaves every other index exactly where it was', () => {
  const block = (index, x) => ({ propId: 'block', index, siteId: null,
    position: [x, 0, 0], rotation: [0, 0, 0], scale: 1 });
  const layout = [block(0, 0), block(3, 3), block(7, 7)];
  const left = removePlacement(layout, 3);
  assert.deepEqual(left.map(entry => entry.index), [0, 7], '只少一件，其它序号一个都不许动');
  assert.deepEqual(left[1].position, [7, 0, 0]);
  assert.deepEqual(removePlacement(layout, 0).map(entry => entry.index), [3, 7]);
  assert.deepEqual(removePlacement(layout, 7).map(entry => entry.index), [0, 3]);
  // 删掉的那一件空出了序号，因此下一次摆放会回填它（而不是继续往后加）。
  assert.equal(nextPlacementIndex(removePlacement(layout, 3)), 1);
  // 匹配不到任何一件：原样返回同一份净化后的布局，长度不变。
  assert.deepEqual(removePlacement(layout, 99).map(entry => entry.index), [0, 3, 7]);
  assert.deepEqual(removePlacement(layout, undefined).map(entry => entry.index), [0, 3, 7]);
  assert.deepEqual(removePlacement(layout, '3').map(entry => entry.index), [0, 3, 7]);
  assert.deepEqual(removePlacement(layout, null).map(entry => entry.index), [0, 3, 7]);
  // 脏输入 / 脏布局：回答空布局，绝不抛错，也不复制不该留的字段。
  for (const bad of [null, undefined, 42, 'blocks', {}, [[]]]) {
    assert.deepEqual(removePlacement(bad, 0), [], `${String(bad)} 必须退化成空布局`);
  }
  assert.deepEqual(removePlacement([{ propId: 'block' }, block(0, 0)], 5).map(entry => entry.index), [0],
    '不合法的一件在删除时同样被净化掉');
  // 输入不被改动：布局是响应式 ref 里的那份数据。
  assert.equal(layout.length, 3);
});

test('a placement keeps a stable identity, and its prop is what the adjust list keys on', () => {
  assert.equal(placementGuid(7), `${PLACEMENT_PREFIX}0007`);
  assert.equal(placementIndex(`${PLACEMENT_PREFIX}0007`), 7);
  assert.equal(placementIndex(`${PLACEMENT_PREFIX}7`), null);
  // The adjust list excludes the world fragment by the entity id storyActors writes:
  // `story.placement.<prop.id>`. The block must stay adjustable, the fragment must not.
  assert.equal(`${PLACEMENT_PREFIX}${findProp('world-fragment').id}`, 'story.placement.world-fragment');
  assert.equal(`${PLACEMENT_PREFIX}${findProp('block').id}`, 'story.placement.block');
  assert.notEqual(findProp('block').id, findProp('world-fragment').id);
});
