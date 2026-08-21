import { editorApi } from '../api/editorApi.js';
import {
  createStoryWorldActorData,
  STORY_WORLD_ACTORS,
  STORY_WORLD_ACTOR_PREFIX,
  STORY_WORLD_CAMERA_SPAWN,
  STORY_WORLD_PLAN_ID,
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

export function isStoryWorldActor(actor = {}) {
  return (
    actorName(actor).startsWith(STORY_WORLD_ACTOR_PREFIX) ||
    String(actor?.source_plan_id || actor?.sourcePlanId || '').trim() === STORY_WORLD_PLAN_ID
  );
}

export function isWorldGeometryActor(actor = {}) {
  if (!actor || typeof actor !== 'object' || actor.visible === false || actor.follow_camera)
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
    const guid = String(actor?.actor_guid || actor?.guid || '').trim();
    if (name) identities.add(`name:${name.toLowerCase()}`);
    if (guid) identities.add(`guid:${guid.toLowerCase()}`);
  }
  return definitions.filter(
    (definition) =>
      !identities.has(`name:${definition.name.toLowerCase()}`) &&
      !identities.has(`guid:${definition.guid.toLowerCase()}`)
  );
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
    };
  }

  const missing = missingStoryWorldActors(initialSnapshot);
  if (missing.length === 0) {
    return {
      generated: false,
      skipped: false,
      managedWorld: true,
      warnings: [],
      createdCount: 0,
      terrainCreated: false,
    };
  }
  const terrainWasMissing = missing.some(
    (definition) => definition.name === STORY_WORLD_TERRAIN_ACTOR.name
  );

  const activeProjectPath = storyProjectPathFromResponse(projectInfoResponse);
  const defaultProjectPath = await safeDefaultProjectPath(api);
  if (isCancelled()) return null;
  const assetRoot = resolveStoryWorldAssetRoot({
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

  const warnings = [];
  const shouldInitializeLighting = classification.kind === 'empty' || terrainWasMissing;
  if (shouldInitializeLighting) {
    onProgress({ status: 'lighting', progress: 15, message: '点亮天光' });
    try {
      const sunlight = await api.sceneTools.sunDirection(activeSceneId, true, [
        ...STORY_WORLD_SUN_DIRECTION,
      ]);
      if (!editorCallSucceeded(sunlight)) {
        throw new Error(editorCallErrorMessage(sunlight, '太阳光初始化失败'));
      }
    } catch (error) {
      throw createBootstrapError(
        '无法启用引擎太阳光与天空环境光，请重试。',
        'LIGHTING_FAILED',
        error
      );
    }
    try {
      const grid = await api.sceneTools.floorGrid(activeSceneId, false);
      if (!editorCallSucceeded(grid)) warnings.push('未能隐藏编辑器地面网格。');
    } catch (_) {
      warnings.push('未能隐藏编辑器地面网格。');
    }
    if (isCancelled()) return null;
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
      96,
      rangeStart + Math.round(((index + 1) / Math.max(missing.length, 1)) * 8)
    );
    onProgress({ status: definition.phase, progress, message: phaseDetails(definition.phase)[2] });
  }

  onProgress({ status: 'complete', progress: 100, message: '云溪村已就绪' });
  return {
    generated: createdCount > 0,
    skipped: false,
    managedWorld: true,
    warnings,
    createdCount,
    terrainCreated,
    assetRoot,
  };
}
