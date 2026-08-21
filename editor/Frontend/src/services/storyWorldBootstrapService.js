import { editorApi } from '../api/editorApi.js';
import {
  createStoryWorldActorData,
  storyWorldExpectedSize,
  storyWorldFinalScale,
  STORY_WORLD_ACTORS,
  STORY_WORLD_ACTOR_PREFIX,
  STORY_WORLD_CAMERA_SPAWN,
  STORY_WORLD_PLAN_ID,
  STORY_WORLD_SCENE_VERSION,
  STORY_WORLD_SUN_DIRECTION,
  STORY_WORLD_TERRAIN_ACTOR,
} from '../config/storyWorld.js';
import { resolveSceneSnapshot } from '../utils/nativeSceneViewport.js';

const IGNORED_ACTOR_TYPES = new Set([
  'audio',
  'camera',
  'follow_camera',
  'light',
  'ui',
  'ui_image',
]);
const STORY_WORLD_LEGACY_SIZE_RATIO = 0.35;
const STORY_WORLD_VALIDATION_LIMITS = Object.freeze({
  terrainX: 100,
  terrainZ: 100,
  lakeX: 40,
  lakeZ: 25,
  houseY: 6,
  sceneY: 8,
});

export function normalizeStoryWorldPath(value) {
  return String(value || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '');
}

export function unwrapStoryWorldResponse(response) {
  let current = response;
  for (let depth = 0; depth < 3; depth += 1) {
    if (!current || typeof current !== 'object' || Array.isArray(current)) break;
    if (!current.data || typeof current.data !== 'object' || Array.isArray(current.data)) break;
    current = current.data;
  }
  return current && typeof current === 'object' && !Array.isArray(current) ? current : {};
}

export function storyProjectModeFromResponse(response) {
  const candidates = [response, response?.data, response?.data?.data];
  for (const candidate of candidates) {
    const mode = String(candidate?.mode || candidate?.world_mode || candidate?.worldMode || '')
      .trim()
      .toLowerCase();
    if (mode) return mode;
  }
  return '';
}

export function storyProjectPathFromResponse(response) {
  const candidates = [response, response?.data, response?.data?.data];
  for (const candidate of candidates) {
    if (typeof candidate === 'string') {
      const directPath = normalizeStoryWorldPath(candidate);
      if (directPath) return directPath;
      continue;
    }
    const projectPath = normalizeStoryWorldPath(
      candidate?.project_path || candidate?.projectPath || candidate?.path
    );
    if (projectPath) return projectPath;
  }
  return '';
}

function editorRootFromFrontendLocation(frontendLocation) {
  let value = String(frontendLocation || '').trim();
  if (!value || /^https?:\/\//i.test(value)) return '';
  value = value.split('#')[0].split('?')[0];
  try {
    value = decodeURIComponent(value);
  } catch (_) {
    // Keep the original path when it contains malformed escape sequences.
  }
  value = value.replace(/^file:\/\/\/?/i, '');
  if (/^\/[A-Za-z]:\//.test(value)) value = value.slice(1);
  value = normalizeStoryWorldPath(value);
  const markerIndex = value.toLowerCase().lastIndexOf('/frontend/');
  return markerIndex >= 0 ? value.slice(0, markerIndex) : '';
}

function editorRootFromProjectPath(projectPath) {
  const value = normalizeStoryWorldPath(projectPath);
  if (!value) return '';
  const lower = value.toLowerCase();
  for (const marker of ['/editor/data/', '/cabbageeditor/data/']) {
    const markerIndex = lower.indexOf(marker);
    if (markerIndex >= 0) return value.slice(0, markerIndex + marker.indexOf('/data/'));
  }
  if (/(?:\/editor|\/cabbageeditor)\/data$/i.test(value)) return value.replace(/\/data$/i, '');
  return '';
}

export function resolveStoryWorldAssetRoot({
  frontendLocation = '',
  activeProjectPath = '',
  defaultProjectPath = '',
} = {}) {
  const roots = [
    editorRootFromFrontendLocation(frontendLocation),
    editorRootFromProjectPath(activeProjectPath),
    editorRootFromProjectPath(defaultProjectPath),
  ].filter(Boolean);
  return roots.length > 0 ? `${normalizeStoryWorldPath(roots[0])}/assets/story_mode` : '';
}

function actorName(actor) {
  return String(actor?.name || actor?.actor_name || '').trim();
}

function actorGuid(actor) {
  return String(actor?.actor_guid || actor?.guid || '').trim();
}

function finiteVector(value, fallback = [0, 0, 0]) {
  if (Array.isArray(value) && value.length >= 3) {
    const vector = value.slice(0, 3).map(Number);
    if (vector.every(Number.isFinite)) return vector;
  }
  if (value && typeof value === 'object') {
    const vector = [value.x, value.y, value.z].map(Number);
    if (vector.every(Number.isFinite)) return vector;
  }
  return [...fallback];
}

export function normalizeStoryWorldAabb(value) {
  const source = Array.isArray(value)
    ? value
    : value && typeof value === 'object'
      ? [value.min_x, value.min_y, value.min_z, value.max_x, value.max_y, value.max_z]
      : [];
  if (source.length < 6) return null;
  const aabb = source.slice(0, 6).map(Number);
  if (!aabb.every(Number.isFinite)) return null;
  return [
    Math.min(aabb[0], aabb[3]),
    Math.min(aabb[1], aabb[4]),
    Math.min(aabb[2], aabb[5]),
    Math.max(aabb[0], aabb[3]),
    Math.max(aabb[1], aabb[4]),
    Math.max(aabb[2], aabb[5]),
  ];
}

export function storyWorldAabbSize(value) {
  const aabb = normalizeStoryWorldAabb(value);
  return aabb ? [aabb[3] - aabb[0], aabb[4] - aabb[1], aabb[5] - aabb[2]] : null;
}

function actorWorldAabb(actor) {
  return normalizeStoryWorldAabb(
    actor?.world_aabb ||
      actor?.worldAabb ||
      actor?.geometry?.world_aabb ||
      actor?.geometry?.worldAabb
  );
}

function actorScale(actor) {
  return finiteVector(actor?.geometry?.scale || actor?.scale, [1, 1, 1]);
}

function actorSceneVersion(actor) {
  const version = Number(actor?.source_scene_version ?? actor?.sourceSceneVersion ?? 1);
  return Number.isFinite(version) ? Math.max(1, Math.trunc(version)) : 1;
}

function actorVisible(actor) {
  return actor?.visible !== false && actor?.optics?.visible !== false;
}

export function isStoryWorldActor(actor = {}) {
  return (
    actorName(actor).startsWith(STORY_WORLD_ACTOR_PREFIX) ||
    String(actor?.source_plan_id || actor?.sourcePlanId || '').trim() === STORY_WORLD_PLAN_ID
  );
}

export function isWorldGeometryActor(actor = {}) {
  if (!actor || typeof actor !== 'object' || !actorVisible(actor) || actor.follow_camera)
    return false;
  const actorType = String(actor.actor_type || actor.type || actor.entity_type || '')
    .trim()
    .toLowerCase();
  return !IGNORED_ACTOR_TYPES.has(actorType);
}

export function classifyStoryWorldScene(snapshot = {}) {
  const scene = resolveSceneSnapshot(snapshot);
  const actors = Array.isArray(scene.actors) ? scene.actors : [];
  const storyActors = actors.filter(isStoryWorldActor);
  const userWorldActors = actors.filter(
    (actor) => isWorldGeometryActor(actor) && !isStoryWorldActor(actor)
  );
  if (storyActors.length > 0) {
    return { kind: 'partial', storyActors, userWorldActors, actors };
  }
  if (userWorldActors.length > 0) {
    return { kind: 'existing', storyActors, userWorldActors, actors };
  }
  return { kind: 'empty', storyActors, userWorldActors, actors };
}

export function missingStoryWorldActors(snapshot = {}, definitions = STORY_WORLD_ACTORS) {
  const scene = resolveSceneSnapshot(snapshot);
  const actors = Array.isArray(scene.actors) ? scene.actors : [];
  const identities = new Set();
  for (const actor of actors) {
    const name = actorName(actor);
    const guid = actorGuid(actor);
    if (name) identities.add(`name:${name.toLowerCase()}`);
    if (guid) identities.add(`guid:${guid.toLowerCase()}`);
  }
  return definitions.filter(
    (definition) =>
      !identities.has(`name:${definition.name.toLowerCase()}`) &&
      !identities.has(`guid:${definition.guid.toLowerCase()}`)
  );
}

function storyActorForDefinition(snapshot, definition) {
  const scene = resolveSceneSnapshot(snapshot);
  const actors = Array.isArray(scene.actors) ? scene.actors : [];
  const expectedName = definition.name.toLowerCase();
  const expectedGuid = definition.guid.toLowerCase();
  return actors.find(
    (actor) =>
      actorName(actor).toLowerCase() === expectedName ||
      actorGuid(actor).toLowerCase() === expectedGuid
  );
}

export function storyWorldMigrationForActor(actor, definition) {
  if (!actor || !definition || !isStoryWorldActor(actor)) return null;
  const currentVersion = actorSceneVersion(actor);
  if (currentVersion >= STORY_WORLD_SCENE_VERSION) return null;

  const currentScale = actorScale(actor);
  const actualSize = storyWorldAabbSize(actorWorldAabb(actor));
  const expectedSize = storyWorldExpectedSize(definition);
  const actualMax = actualSize ? Math.max(...actualSize) : 0;
  const expectedMax = Math.max(...expectedSize);
  const expectedFinalScale = storyWorldFinalScale(definition);
  const currentScaleMax = Math.max(...currentScale.map((value) => Math.abs(value)));
  const expectedScaleMax = Math.max(...expectedFinalScale.map((value) => Math.abs(value)));
  const normalizedLegacySize =
    expectedMax > 0 &&
    (!actualSize ||
      actualMax < expectedMax * STORY_WORLD_LEGACY_SIZE_RATIO ||
      (currentVersion <= 1 && currentScaleMax < expectedScaleMax * 0.75));
  const scale = normalizedLegacySize
    ? currentScale.map((value) => value * definition.importScale)
    : currentScale;

  return {
    actor,
    definition,
    currentVersion,
    actualSize,
    expectedSize,
    scale,
    repaired: normalizedLegacySize,
    needsResourceRebind: currentVersion < STORY_WORLD_SCENE_VERSION,
    actorGuid: actorGuid(actor) || definition.guid,
  };
}

export function storyWorldMigrations(snapshot, definitions = STORY_WORLD_ACTORS) {
  return definitions
    .map((definition) =>
      storyWorldMigrationForActor(storyActorForDefinition(snapshot, definition), definition)
    )
    .filter(Boolean);
}

export function createStoryWorldMigrationActorData(migration) {
  const { definition } = migration;
  return {
    actor_name: definition.name,
    name: definition.name,
    actor_guid: migration.actorGuid || definition.guid,
    ...(migration.repaired ? { scale: [...migration.scale] } : {}),
    source_plan_id: STORY_WORLD_PLAN_ID,
    source_scene_version: STORY_WORLD_SCENE_VERSION,
    skip_if_exists: true,
    update_if_exists: true,
  };
}

function unionAabbs(aabbs) {
  const valid = aabbs.map(normalizeStoryWorldAabb).filter(Boolean);
  if (valid.length === 0) return null;
  return valid.reduce(
    (result, aabb) => [
      Math.min(result[0], aabb[0]),
      Math.min(result[1], aabb[1]),
      Math.min(result[2], aabb[2]),
      Math.max(result[3], aabb[3]),
      Math.max(result[4], aabb[4]),
      Math.max(result[5], aabb[5]),
    ],
    [...valid[0]]
  );
}

export function validateStoryWorldSnapshot(snapshot = {}) {
  const scene = resolveSceneSnapshot(snapshot);
  const actors = Array.isArray(scene.actors) ? scene.actors : [];
  const terrain = storyActorForDefinition(scene, STORY_WORLD_TERRAIN_ACTOR);
  const lakeDefinition = STORY_WORLD_ACTORS.find(
    (definition) => definition.name === 'StoryWorld_YunxiLake'
  );
  const lake = lakeDefinition ? storyActorForDefinition(scene, lakeDefinition) : null;
  const houses = actors.filter(
    (actor) => isStoryWorldActor(actor) && actorName(actor).startsWith('StoryWorld_House_')
  );
  const terrainSize = storyWorldAabbSize(actorWorldAabb(terrain));
  const lakeSize = storyWorldAabbSize(actorWorldAabb(lake));
  const houseHeights = houses
    .filter(actorVisible)
    .map((actor) => storyWorldAabbSize(actorWorldAabb(actor))?.[1])
    .filter(Number.isFinite);
  const worldBounds =
    unionAabbs(actors.filter(isStoryWorldActor).map(actorWorldAabb)) ||
    normalizeStoryWorldAabb(scene.scene_aabb || scene.sceneAabb || scene.world_aabb);
  const sceneSize = storyWorldAabbSize(worldBounds);
  const errors = [];

  if (!terrain || !actorVisible(terrain) || !terrainSize) errors.push('基础地形不可见或边界无效');
  else if (
    terrainSize[0] < STORY_WORLD_VALIDATION_LIMITS.terrainX ||
    terrainSize[2] < STORY_WORLD_VALIDATION_LIMITS.terrainZ
  ) {
    errors.push('基础地形尺寸不足');
  }
  if (!lake || !actorVisible(lake) || !lakeSize) errors.push('云溪湖不可见或边界无效');
  else if (
    lakeSize[0] < STORY_WORLD_VALIDATION_LIMITS.lakeX ||
    lakeSize[2] < STORY_WORLD_VALIDATION_LIMITS.lakeZ
  ) {
    errors.push('云溪湖尺寸不足');
  }
  if (!houseHeights.some((height) => height >= STORY_WORLD_VALIDATION_LIMITS.houseY)) {
    errors.push('村落建筑尺寸不足');
  }
  if (!sceneSize || sceneSize[1] < STORY_WORLD_VALIDATION_LIMITS.sceneY) {
    errors.push('场景垂直跨度不足');
  }

  return {
    valid: errors.length === 0,
    errors,
    worldBounds,
    metrics: {
      terrainSize,
      lakeSize,
      maximumHouseHeight: houseHeights.length > 0 ? Math.max(...houseHeights) : 0,
      sceneSize,
    },
  };
}

export function editorCallSucceeded(response) {
  if (response === false || response?.success === false || response?.ok === false) return false;
  const payload = unwrapStoryWorldResponse(response);
  return String(payload?.status || '').toLowerCase() !== 'error';
}

export function editorCallErrorMessage(response, fallback) {
  const payload = unwrapStoryWorldResponse(response);
  return String(payload?.message || response?.message || fallback || '引擎操作失败').trim();
}

function assetPath(root, filename) {
  return `${normalizeStoryWorldPath(root)}/${filename}`;
}

function phaseDetails(phase) {
  const phases = {
    water: ['water', 44, '引水入村'],
    roads: ['roads', 58, '铺设村道'],
    village: ['village', 74, '搭建云溪村'],
    decorations: ['decorations', 88, '点缀山林'],
  };
  return phases[phase] || phases.village;
}

function createBootstrapError(message, code, cause = null) {
  const error = new Error(message, cause ? { cause } : undefined);
  error.code = code;
  return error;
}

async function safeDefaultProjectPath(api) {
  try {
    return storyProjectPathFromResponse(await api.project.getDefaultProjectPath());
  } catch (error) {
    console.warn('[StoryMode] failed to resolve default project path', error);
    return '';
  }
}

async function enableStoryWorldLighting(api, sceneId, warnings) {
  const sunlight = await api.sceneTools.sunDirection(sceneId, true, [...STORY_WORLD_SUN_DIRECTION]);
  if (!editorCallSucceeded(sunlight)) {
    throw new Error(editorCallErrorMessage(sunlight, '太阳光初始化失败'));
  }
  try {
    const grid = await api.sceneTools.floorGrid(sceneId, false);
    if (!editorCallSucceeded(grid)) warnings.push('未能隐藏编辑器地面网格。');
  } catch (_) {
    warnings.push('未能隐藏编辑器地面网格。');
  }
}

export async function runStoryWorldBootstrap({
  api = editorApi,
  sceneId,
  frontendLocation = globalThis.window?.location?.href || '',
  setCameraPose = null,
  onProgress = () => {},
  isCancelled = () => false,
} = {}) {
  const activeSceneId = String(sceneId || '').trim();
  if (!activeSceneId) throw createBootstrapError('当前剧情场景不可用。', 'SCENE_UNAVAILABLE');

  onProgress({ status: 'checking', progress: 5, message: '检查云溪村世界状态' });
  const projectInfoResponse = await api.projectSettings.getActiveProjectInfo();
  if (isCancelled()) return null;
  const projectMode = storyProjectModeFromResponse(projectInfoResponse);
  if (!projectMode) {
    throw createBootstrapError(
      '无法确认当前项目模式，为保护已有世界已停止自动生成。',
      'MODE_UNKNOWN'
    );
  }
  if (projectMode !== 'story') {
    return {
      generated: false,
      skipped: true,
      managedWorld: false,
      skipReason: 'not-story',
      warnings: [],
      migrationWarnings: [],
      repairedCount: 0,
      validation: null,
      worldBounds: null,
    };
  }

  const initialSnapshotResponse = await api.scene.getSnapshot(activeSceneId);
  if (isCancelled()) return null;
  const initialSnapshot = resolveSceneSnapshot(initialSnapshotResponse);
  const classification = classifyStoryWorldScene(initialSnapshot);
  if (classification.kind === 'existing') {
    return {
      generated: false,
      skipped: true,
      managedWorld: false,
      skipReason: 'existing-world',
      warnings: [],
      migrationWarnings: [],
      repairedCount: 0,
      validation: null,
      worldBounds: normalizeStoryWorldAabb(initialSnapshot.scene_aabb),
    };
  }

  const missing = missingStoryWorldActors(initialSnapshot);
  const migrations = storyWorldMigrations(initialSnapshot);
  const terrainWasMissing = missing.some(
    (definition) => definition.name === STORY_WORLD_TERRAIN_ACTOR.name
  );
  const activeProjectPath = storyProjectPathFromResponse(projectInfoResponse);
  let assetRoot = '';
  if (missing.length > 0 || migrations.length > 0) {
    const defaultProjectPath = await safeDefaultProjectPath(api);
    if (isCancelled()) return null;
    assetRoot = resolveStoryWorldAssetRoot({
      frontendLocation,
      activeProjectPath,
      defaultProjectPath,
    });
    if (!assetRoot) {
      throw createBootstrapError(
        '无法定位剧情模式山水村落资源，请检查编辑器安装目录。',
        'ASSET_ROOT_UNAVAILABLE'
      );
    }
  }

  const warnings = [];
  const migrationWarnings = [];
  const shouldInitializeLighting =
    classification.kind === 'empty' || terrainWasMissing || migrations.length > 0;
  if (shouldInitializeLighting) {
    onProgress({ status: 'lighting', progress: 15, message: '点亮天光' });
    try {
      await enableStoryWorldLighting(api, activeSceneId, warnings);
    } catch (error) {
      throw createBootstrapError(
        '无法启用引擎太阳光与天空环境光，请重试。',
        'LIGHTING_FAILED',
        error
      );
    }
    if (isCancelled()) return null;
  }

  let repairedCount = 0;
  let upgradedCount = 0;
  if (migrations.length > 0) {
    onProgress({ status: 'repairing', progress: 22, message: '升级云溪村模型与材质' });
    for (const migration of migrations) {
      if (isCancelled()) return null;
      try {
        const resourcePath = assetPath(assetRoot, migration.definition.asset);
        if (migration.needsResourceRebind) {
          if (typeof api.sceneTools.rebindActorResource !== 'function') {
            throw new Error('当前编辑器不支持剧情资源重新绑定。');
          }
          const rebound = await api.sceneTools.rebindActorResource(
            activeSceneId,
            migration.actorGuid,
            resourcePath
          );
          if (!editorCallSucceeded(rebound)) {
            throw new Error(
              editorCallErrorMessage(rebound, `升级 ${migration.definition.name} 模型失败`)
            );
          }
          upgradedCount += 1;
        }

        const updated = await api.sceneTools.createActor(
          activeSceneId,
          resourcePath,
          'model',
          createStoryWorldMigrationActorData(migration)
        );
        if (!editorCallSucceeded(updated)) {
          throw new Error(
            editorCallErrorMessage(updated, `更新 ${migration.definition.name} 版本失败`)
          );
        }
        if (migration.repaired) repairedCount += 1;
      } catch (error) {
        const warning = `${migration.definition.name} 模型升级失败，将在下次进入时重试。`;
        if (migration.definition.critical) {
          throw createBootstrapError(
            '基础地形模型升级失败，请重试。',
            'WORLD_MIGRATION_FAILED',
            error
          );
        }
        migrationWarnings.push(warning);
      }
    }
  }

  let terrainCreated = false;
  let createdCount = 0;
  let currentPhase = '';
  for (let index = 0; index < missing.length; index += 1) {
    if (isCancelled()) return null;
    const definition = missing[index];
    if (definition.phase === 'terrain') {
      onProgress({ status: 'terrain', progress: 28, message: '铺设山川' });
    } else if (definition.phase !== currentPhase) {
      currentPhase = definition.phase;
      const [status, progress, message] = phaseDetails(definition.phase);
      onProgress({ status, progress, message });
    }

    try {
      const created = await api.sceneTools.createActor(
        activeSceneId,
        assetPath(assetRoot, definition.asset),
        'model',
        createStoryWorldActorData(definition)
      );
      if (!editorCallSucceeded(created)) {
        throw new Error(editorCallErrorMessage(created, `创建 ${definition.name} 失败`));
      }
      createdCount += 1;
      if (definition.name === STORY_WORLD_TERRAIN_ACTOR.name) terrainCreated = true;

      if (definition.physics?.physics_enabled) {
        try {
          const physics = await api.sceneTools.setActorPhysics(
            activeSceneId,
            definition.name,
            definition.physics
          );
          if (!editorCallSucceeded(physics)) {
            warnings.push(`${definition.name} 的底层物理碰撞未能启用。`);
          }
        } catch (_) {
          warnings.push(`${definition.name} 的底层物理碰撞未能启用。`);
        }
      }

      if (
        definition.name === STORY_WORLD_TERRAIN_ACTOR.name &&
        terrainWasMissing &&
        terrainCreated &&
        typeof setCameraPose === 'function'
      ) {
        try {
          await setCameraPose(STORY_WORLD_CAMERA_SPAWN, { persist: true });
        } catch (error) {
          console.warn('[StoryMode] failed to set Story World spawn camera', error);
          warnings.push('村落已生成，但未能设置首次出生视角。');
        }
      }
    } catch (error) {
      if (definition.critical) {
        throw createBootstrapError(
          '基础地形创建失败，请检查剧情资源后重试。',
          'TERRAIN_FAILED',
          error
        );
      }
      warnings.push(`${definition.name} 创建失败，将在下次进入时继续补齐。`);
    }

    const rangeStart = definition.phase === 'decorations' ? 88 : 38;
    const progress = Math.min(
      94,
      rangeStart + Math.round(((index + 1) / Math.max(missing.length, 1)) * 8)
    );
    onProgress({ status: definition.phase, progress, message: phaseDetails(definition.phase)[2] });
  }

  if (isCancelled()) return null;
  onProgress({ status: 'validating', progress: 97, message: '确认世界画面' });
  const finalSnapshotResponse = await api.scene.getSnapshot(activeSceneId);
  if (isCancelled()) return null;
  const finalSnapshot = resolveSceneSnapshot(finalSnapshotResponse);
  const validation = validateStoryWorldSnapshot(finalSnapshot);
  if (!validation.valid) {
    const error = createBootstrapError(
      '剧情资源尺寸异常，世界修复未完成。',
      'WORLD_VALIDATION_FAILED'
    );
    error.validation = validation;
    throw error;
  }

  onProgress({ status: 'complete', progress: 100, message: '云溪村已就绪' });
  return {
    generated: createdCount > 0,
    skipped: false,
    managedWorld: true,
    warnings: [...warnings, ...migrationWarnings],
    migrationWarnings,
    createdCount,
    repairedCount,
    upgradedCount,
    terrainCreated,
    validation,
    worldBounds: validation.worldBounds,
    assetRoot,
  };
}
