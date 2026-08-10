import { reactive } from 'vue';
import { useDockStore } from '@/stores/dockStore.js';
import { closeFloatingPanel, openFloatingPanel } from '@/utils/panelWindows.js';
import { getPluginManifest } from '@/config/pluginManifest.js';

const PANEL_ZONES = Object.freeze({
  MainPage: 'center',
  SceneTools: 'right',
  Object: 'right',
  EditorSettings: 'right',
  // Keep node guidance in the main-page DOM so the highlight can target nodes,
  // ports and blocks without falling back to the legacy bottom dock.
  NodeGraphPanel: 'center',
});

const SELECTOR_KEYS = Object.freeze({
  'main-viewport': '[data-guidance="main-viewport"]',
  'scene-shortcut': '[data-guidance="scene-shortcut"]',
  'node-shortcut': '[data-guidance="node-shortcut"]',
  'scene-import-model': '[data-guidance="scene-import-model"]',
  'scene-actor-list': '[data-guidance="scene-actor-list"]',
  'scene-lighting': '[data-guidance="scene-lighting"]',
  'scene-light-x': '[data-guidance="scene-light-x"]',
  'preview-start': '[data-guidance="preview-start"]',
  'preview-stop': '[data-guidance="preview-stop"]',
  'settings-viewport': '[data-guidance="settings-viewport"]',
  'settings-viewport-ui': '[data-guidance="settings-viewport-ui"]',
  'settings-camera-speed': '[data-guidance="settings-camera-speed"]',
  'settings-grid': '[data-guidance="settings-grid"]',
  'object-transform': '[data-guidance="object-transform"]',
  'object-position-x': '[data-guidance="object-position-x"]',
  'object-rotation-y': '[data-guidance="object-rotation-y"]',
  'object-scale-x': '[data-guidance="object-scale-x"]',
  'object-physics': '[data-guidance="object-physics"]',
  'object-physics-enabled': '[data-guidance="object-physics-enabled"]',
  'object-physics-mass': '[data-guidance="object-physics-mass"]',
  'node-run': '[data-guidance="node-run"]',
  'node-select-mode': '[data-guidance="node-select-mode"]',
  'node-delete-mode': '[data-guidance="node-delete-mode"]',
  'node-toolbox': '[data-guidance="node-toolbox"]',
  'node-state-tool': '[data-guidance="node-state-tool"]',
  'node-type-custom': '[data-guidance="node-type-custom"]',
  'node-type-start': '[data-guidance="node-type-start"]',
  'node-canvas': '[data-guidance="node-canvas"]',
  'node-blockly-editor': '[data-guidance="node-blockly-editor"]',
  'node-transition-condition': '[data-guidance="node-transition-condition"]',
  'cabbage-chat-input': '[data-guidance="cabbage-chat-input"]',
  'cabbage-chat-send': '[data-guidance="cabbage-chat-send"]',
});

const state = reactive({
  active: false,
  guidance: null,
  stepIndex: 0,
  targetRect: null,
  fromRect: null,
  preparing: false,
});

let restoreState = null;
let rectTimer = null;
let lifecycleToken = 0;
let exactTargetSeen = false;
let exactTargetMissingTicks = 0;
let exactTargetPrepareTicks = 0;
let fromTargetPrepareTicks = 0;
let lifecycleGuardsAttached = false;

function safeId(value) {
  return String(value || '').replace(/["\\]/g, '\\$&');
}

function clonePanelState(panel) {
  return panel ? {
    open: Boolean(panel.open),
    mode: String(panel.mode || 'docked'),
    dockZone: String(panel.dockZone || ''),
    order: Number(panel.order) || 0,
    width: Number(panel.width) || 0,
    height: Number(panel.height) || 0,
  } : null;
}

function wait(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function targetSelector(target = {}) {
  if (target.kind === 'selector' || target.kind === 'region') {
    return SELECTOR_KEYS[String(target.selectorKey || '')] || '';
  }
  if (target.kind === 'actor' && target.actorName) return `[data-actor-name="${safeId(target.actorName)}"]`;
  if (target.kind === 'node' && target.nodeId) return `[data-node-id="${safeId(target.nodeId)}"]`;
  if (target.kind === 'edge' && target.edgeId) return `[data-edge-id="${safeId(target.edgeId)}"]`;
  if (target.kind === 'block-type' && target.blockType) return `[data-block-type="${safeId(target.blockType)}"]`;
  if (target.kind === 'port' && target.nodeId) {
    const side = target.portSide ? `[data-port-side="${safeId(target.portSide)}"]` : '';
    const index = Number.isFinite(Number(target.portIndex))
      ? `[data-port-index="${safeId(target.portIndex)}"]`
      : '';
    return `[data-node-id="${safeId(target.nodeId)}"]${side}${index}`;
  }
  if (target.kind === 'block' && target.blockId) return `[data-block-id="${safeId(target.blockId)}"]`;
  return '';
}

function fallbackSelector(target = {}) {
  if (target.kind === 'actor') return SELECTOR_KEYS['scene-actor-list'];
  if (['node', 'edge', 'port'].includes(target.kind)) return SELECTOR_KEYS['node-canvas'];
  if (['block', 'block-type'].includes(target.kind)) return SELECTOR_KEYS['node-blockly-editor'];
  return '';
}

function elementForTarget(target = {}, { allowFallback = true } = {}) {
  const selector = targetSelector(target);
  let element = selector ? document.querySelector(selector) : null;
  if (!element && allowFallback) {
    const fallback = fallbackSelector(target);
    element = fallback ? document.querySelector(fallback) : null;
  }
  return element;
}

function rectForTarget(target = {}, { allowFallback = true } = {}) {
  const element = elementForTarget(target, { allowFallback });
  if (!element) return null;
  const rect = element.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return null;
  return {
    left: Math.max(6, rect.left),
    top: Math.max(6, rect.top),
    width: Math.max(20, Math.min(rect.width, window.innerWidth - Math.max(6, rect.left) - 6)),
    height: Math.max(20, Math.min(rect.height, window.innerHeight - Math.max(6, rect.top) - 6)),
  };
}

function currentStep() {
  return state.guidance?.steps?.[state.stepIndex] || null;
}

function exactElementForTarget(target = {}) {
  return elementForTarget(target, { allowFallback: false });
}

function revealTarget(target = {}, { allowFallback = true } = {}) {
  const element = elementForTarget(target, { allowFallback });
  if (!element || typeof element.scrollIntoView !== 'function') return;
  const rect = element.getBoundingClientRect();
  const outsideViewport = rect.top < 8
    || rect.left < 8
    || rect.bottom > window.innerHeight - 8
    || rect.right > window.innerWidth - 8;
  if (outsideViewport) {
    element.scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'smooth' });
  }
}

function guidancePrepareDetail(target = {}) {
  return {
    panelId: state.guidance?.panelId,
    selectorKey: String(target.selectorKey || ''),
    nodeId: String(target.nodeId || ''),
    blockId: String(target.blockId || ''),
    blockType: String(target.blockType || ''),
    edgeId: String(target.edgeId || ''),
    portSide: String(target.portSide || ''),
    portIndex: Number.isFinite(Number(target.portIndex)) ? Number(target.portIndex) : null,
  };
}

function prepareGuidanceTarget(target = {}) {
  if (!target || !Object.keys(target).length) return;
  window.dispatchEvent(new CustomEvent('cabbage-guidance-prepare', {
    detail: guidancePrepareDetail(target),
  }));
}

function refreshRects() {
  if (!state.active) return;
  const panelId = state.guidance?.panelId;
  if (panelId !== 'MainPage') {
    const dockStore = useDockStore();
    const panel = dockStore.panels[panelId];
    if (!panel?.open || panel.mode !== 'docked') {
      void stop({ restorePanelState: false });
      return;
    }
  }

  const step = currentStep();
  const target = step?.target || {};
  const fromTarget = step?.fromTarget || {};
  const exactElement = exactElementForTarget(target);
  if (exactElement) {
    exactTargetSeen = true;
    exactTargetMissingTicks = 0;
    exactTargetPrepareTicks = 0;
  } else if (exactTargetSeen && ['node', 'block', 'edge', 'port'].includes(target.kind)) {
    exactTargetMissingTicks += 1;
    if (exactTargetMissingTicks >= 3) {
      void stop();
      return;
    }
  } else if (['node', 'block', 'edge', 'port'].includes(target.kind)) {
    exactTargetPrepareTicks += 1;
    if (exactTargetPrepareTicks <= 10 && exactTargetPrepareTicks % 3 === 1) {
      prepareGuidanceTarget(target);
    }
  }
  if (Object.keys(fromTarget).length && !exactElementForTarget(fromTarget)) {
    fromTargetPrepareTicks += 1;
    if (fromTargetPrepareTicks <= 10 && fromTargetPrepareTicks % 3 === 1) {
      prepareGuidanceTarget(fromTarget);
    }
  } else {
    fromTargetPrepareTicks = 0;
  }
  state.targetRect = rectForTarget(target);
  // A drag source should never fall back to the destination editor. Hide the blue
  // marker until the exact palette block or source port is visible.
  state.fromRect = rectForTarget(fromTarget, { allowFallback: false });
}

function startRectTracking() {
  stopRectTracking();
  refreshRects();
  rectTimer = window.setInterval(refreshRects, 180);
}

function stopRectTracking() {
  if (rectTimer) window.clearInterval(rectTimer);
  rectTimer = null;
}

async function ensurePanel(panelId, selectorKey = '') {
  if (!panelId) return false;
  if (panelId === 'MainPage') {
    restoreState = null;
    window.dispatchEvent(new CustomEvent('cabbage-guidance-prepare', {
      detail: { panelId, selectorKey },
    }));
    await wait(80);
    const selector = SELECTOR_KEYS[String(selectorKey || '')] || '';
    return selector ? Boolean(document.querySelector(selector)) : true;
  }
  const dockStore = useDockStore();
  const panel = dockStore.panels[panelId];
  if (!panel) return false;
  restoreState = restoreState || { panelId, panel: clonePanelState(panel) };

  if (panel.open && panel.mode === 'external') await closeFloatingPanel(dockStore, panelId);
  dockStore.popIn(panelId);
  dockStore.setDockZone(panelId, PANEL_ZONES[panelId] || panel.dockZone || 'right');

  if (panelId === 'NodeGraphPanel') {
    const manifest = getPluginManifest(panelId);
    const width = Number(manifest?.defaultFloatWidth || manifest?.defaultWidth || panel.width);
    const height = Number(manifest?.defaultFloatHeight || manifest?.defaultHeight || panel.height);
    dockStore.resizePanel(panelId, width, height);
  }

  dockStore.openPanel(panelId);
  window.dispatchEvent(new CustomEvent('cabbage-guidance-prepare', {
    detail: { panelId, selectorKey },
  }));
  window.dispatchEvent(new Event('resize'));
  await wait(panelId === 'NodeGraphPanel' ? 260 : 180);
  return Boolean(dockStore.panels[panelId]?.open && dockStore.panels[panelId]?.mode === 'docked');
}

async function restorePanel() {
  const saved = restoreState;
  restoreState = null;
  if (!saved?.panel) return;
  const dockStore = useDockStore();
  const panel = dockStore.panels[saved.panelId];
  if (!panel) return;

  panel.dockZone = saved.panel.dockZone || panel.dockZone;
  panel.order = saved.panel.order;
  if (saved.panel.width) panel.width = saved.panel.width;
  if (saved.panel.height) panel.height = saved.panel.height;

  if (!saved.panel.open) {
    dockStore.closePanel(saved.panelId);
  } else if (saved.panel.mode === 'external') {
    dockStore.closePanel(saved.panelId);
    await openFloatingPanel(dockStore, saved.panelId);
  } else {
    dockStore.popIn(saved.panelId);
    dockStore.openPanel(saved.panelId);
  }
  window.dispatchEvent(new Event('resize'));
}

function stepFor(selectorKey, action, text, extra = {}) {
  return {
    target: { kind: 'selector', selectorKey },
    action,
    text,
    ...extra,
  };
}

const LEGACY_TUTORIAL_GUIDANCE = Object.freeze({
  'tutorial.import_model': {
    panelId: 'SceneTools',
    steps: [stepFor('scene-import-model', 'click', '打开场景管理中的导入入口，再选择要导入的模型。')],
  },
  'tutorial.transform_model': {
    panelId: 'Object',
    steps: [stepFor('object-transform', 'drag', '展开“变换”，修改位置、旋转或缩放中的任意一个参数。')],
  },
  'tutorial.adjust_lighting': {
    panelId: 'MainPage',
    steps: [stepFor('scene-lighting', 'click', '在页面左上角切换光照，或修改光照方向的任意轴。')],
  },
  'tutorial.adjust_physics': {
    panelId: 'Object',
    steps: [stepFor('object-physics', 'click', '展开“物理”，启用物理或修改质量、弹性、阻尼和锁轴。')],
  },
  'tutorial.create_node': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-canvas', 'drag', '把左侧的状态节点拖到节点编辑区。', {
      fromTarget: { kind: 'selector', selectorKey: 'node-toolbox' },
    })],
  },
  'tutorial.move_node': {
    panelId: 'NodeGraphPanel',
    steps: [{ target: { kind: 'node' }, action: 'drag', text: '按住任意节点并拖到新的位置。' }],
  },
  'tutorial.connect_nodes': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-canvas', 'connect', '从一个节点的输出端口拖向另一个节点的输入端口。')],
  },
  'tutorial.drag_block': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-blockly-editor', 'drag', '先选中节点，再把左侧微观积木拖入节点内部编辑区。', {
      fromTarget: { kind: 'selector', selectorKey: 'node-toolbox' },
    })],
  },
  'tutorial.edit_block_parameter': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-blockly-editor', 'click', '选中节点内部的积木，然后修改它的下拉项、数值或文本参数。')],
  },
  'tutorial.set_transition_condition': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-transition-condition', 'connect', '选中一条连线，在条件编辑区接入一个返回 Boolean 的条件积木。')],
  },
  'tutorial.run_node_graph': {
    panelId: 'NodeGraphPanel',
    steps: [stepFor('node-run', 'click', '点击节点 Dock 顶部的“运行”按钮。展示不会真的启动逻辑。')],
  },
});


function taskGuidanceText(source, fallback, english = false) {
  const fields = english
    ? ['guidanceTextEn', 'suggestionEn', 'completionCriteriaEn', 'messageEn']
    : ['guidanceText', 'suggestion', 'completionCriteria', 'message'];
  const value = fields.map((field) => String(source?.[field] || '').trim()).find(Boolean);
  return value || String(fallback || '');
}

function targetFromBinding(bindings, key, fallback) {
  const value = String(bindings?.[key] || '');
  if (!value) return fallback;
  if (key === 'modelActorName') return { kind: 'actor', actorName: value };
  if (key === 'edgeId') return { kind: 'edge', edgeId: value };
  if (key.endsWith('BlockId')) return { kind: 'block', blockId: value };
  return { kind: 'node', nodeId: value };
}

function tutorialStep(source, target, action, fallback, extra = {}) {
  const { fallbackEn = fallback, preferFallbackText = false, ...stepExtra } = extra;
  return {
    target,
    action,
    text: preferFallbackText ? String(fallback || '') : taskGuidanceText(source, fallback),
    textEn: preferFallbackText ? String(fallbackEn || fallback || '') : taskGuidanceText(source, fallbackEn, true),
    ...stepExtra,
  };
}

function basicGuidance(panelId, target, action, fallback, extra = {}) {
  return (source) => ({ panelId, steps: [tutorialStep(source, target, action, fallback, extra)] });
}

const BASICS_TUTORIAL_GUIDANCE = Object.freeze({
  focus_viewport: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'click', 'Click the 3D viewport once.'),
  move_camera_forward_back: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'key', 'Focus the viewport, then press W or S until the camera moves.'),
  move_camera_left_right: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'key', 'Focus the viewport, then press A or D until the camera moves.'),
  move_camera_up_down: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'key', 'Focus the viewport, then press Q or E until the camera moves.'),
  rotate_camera: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'drag', 'Hold the right mouse button in the viewport and drag until the camera rotates.'),
  move_camera_wheel: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'main-viewport' }, 'wheel', 'Scroll the mouse wheel over the viewport until the camera moves.'),
  open_scene_manager: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'scene-shortcut' }, 'click', 'Click the Scene Manager shortcut yourself.'),
  import_model: basicGuidance('SceneTools', { kind: 'selector', selectorKey: 'scene-import-model' }, 'click', 'Import a model and wait for it to appear in the scene.'),
  select_model: (source) => ({ panelId: 'SceneTools', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'modelActorName', { kind: 'selector', selectorKey: 'scene-actor-list' }),
    'click',
    'Select the tutorial model in the scene tree or viewport.'
  )] }),
  set_position_x: basicGuidance('Object', { kind: 'selector', selectorKey: 'object-position-x' }, 'input', 'Set Position X to 1.'),
  set_rotation_y: basicGuidance('Object', { kind: 'selector', selectorKey: 'object-rotation-y' }, 'input', 'Set Rotation Y to 45.'),
  set_scale_x: basicGuidance('Object', { kind: 'selector', selectorKey: 'object-scale-x' }, 'input', 'Set Scale X to 1.5.'),
  enable_physics: basicGuidance('Object', { kind: 'selector', selectorKey: 'object-physics-enabled' }, 'click', 'Enable Physics Simulation.'),
  set_mass: basicGuidance('Object', { kind: 'selector', selectorKey: 'object-physics-mass' }, 'input', 'Set Mass to 10.'),
  set_light_x: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'scene-light-x' }, 'input', 'Set Scene Lighting Direction X to 0.5.'),
  open_nodes: basicGuidance('MainPage', { kind: 'selector', selectorKey: 'node-shortcut' }, 'click', 'Click the Nodes shortcut yourself.'),
  confirm_start_node: (source) => {
    const startNodeId = String(source.bindings?.startNodeId || '');
    if (startNodeId) {
      return { panelId: 'NodeGraphPanel', steps: [tutorialStep(
        source,
        { kind: 'node', nodeId: startNodeId },
        'click',
        '点击黄色高亮的“开始”节点，确认中间画布里只有这一个开始节点。',
        { fallbackEn: 'Click the yellow-highlighted Start node and confirm it is the only Start node on the canvas.', preferFallbackText: true },
      )] };
    }
    return { panelId: 'NodeGraphPanel', steps: [
      tutorialStep(
        source,
        { kind: 'selector', selectorKey: 'node-canvas' },
        'drag',
        '从蓝色高亮的“状态节点”开始拖动，把它放到黄色高亮的中间画布空白处。',
        {
          fromTarget: { kind: 'selector', selectorKey: 'node-state-tool' },
          fallbackEn: 'Drag the blue-highlighted State Node into an empty spot in the yellow-highlighted middle canvas.',
          preferFallbackText: true,
        },
      ),
      tutorialStep(
        source,
        { kind: 'selector', selectorKey: 'node-type-start' },
        'click',
        '保持新节点被选中，再点击右侧黄色高亮的“开始节点”。如果它已经显示“开始”，就不用再改。',
        { fallbackEn: 'Keep the new node selected, then click the yellow-highlighted Start Node option on the right. If it already says Start, leave it unchanged.', preferFallbackText: true },
      ),
    ] };
  },
  create_custom_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    { kind: 'selector', selectorKey: 'node-canvas' },
    'drag',
    '从蓝色高亮的“状态节点”开始拖动，把它放到黄色高亮的中间画布空白处，新节点应显示为“自定义节点”。',
    {
      fromTarget: { kind: 'selector', selectorKey: 'node-state-tool' },
      fallbackEn: 'Drag the blue-highlighted State Node into an empty spot in the yellow-highlighted canvas. The new node should say Custom Node.',
      preferFallbackText: true,
    }
  )] }),
  select_custom_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'customNodeId', { kind: 'selector', selectorKey: 'node-canvas' }),
    'click',
    String(source.message || ''),
    { fallbackEn: String(source.messageEn || ''), preferFallbackText: true }
  )] }),
  move_custom_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'customNodeId', { kind: 'selector', selectorKey: 'node-canvas' }),
    'drag',
    '按住黄色高亮的自定义节点标题区，把它明显拖到另一个位置后松开。',
    { fallbackEn: 'Hold the title area of the yellow-highlighted Custom node, drag it a visible distance, then release.', preferFallbackText: true }
  )] }),
  create_delete_practice_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    { kind: 'selector', selectorKey: 'node-canvas' },
    'drag',
    String(source.message || ''),
    {
      fromTarget: { kind: 'selector', selectorKey: 'node-state-tool' },
      fallbackEn: String(source.messageEn || ''),
      preferFallbackText: true,
    }
  )] }),
  delete_practice_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'node-delete-mode' },
      'click',
      '先点击黄色高亮的“清除”按钮。',
      { fallbackEn: 'First click the yellow-highlighted Clear button.', preferFallbackText: true },
    ),
    tutorialStep(
      source,
      targetFromBinding(source.bindings, 'deletePracticeNodeId', { kind: 'selector', selectorKey: 'node-canvas' }),
      'click',
      '再点击黄色高亮的临时练习节点，把它从画布中删除。',
      { fallbackEn: 'Then click the yellow-highlighted temporary practice node to delete it from the canvas.', preferFallbackText: true },
    ),
  ] }),
  return_select_tool: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    { kind: 'selector', selectorKey: 'node-select-mode' },
    'click',
    String(source.message || ''),
    { fallbackEn: String(source.messageEn || ''), preferFallbackText: true }
  )] }),
  connect_nodes: (source) => {
    const startNodeId = String(source.bindings?.startNodeId || '');
    const customNodeId = String(source.bindings?.customNodeId || '');
    const target = startNodeId && customNodeId
      ? { kind: 'port', nodeId: customNodeId, portSide: 'left', portIndex: 0 }
      : { kind: 'selector', selectorKey: 'node-canvas' };
    const extra = startNodeId && customNodeId
      ? { fromTarget: { kind: 'port', nodeId: startNodeId, portSide: 'right', portIndex: 0 } }
      : {};
    return {
      panelId: 'NodeGraphPanel',
      steps: [tutorialStep(
        source,
        target,
        'connect',
        '从蓝色高亮的开始节点右侧小圆点开始，连到黄色高亮的自定义节点左侧小圆点。',
        {
          ...extra,
          fallbackEn: 'Connect the blue-highlighted circle on the right of Start to the yellow-highlighted circle on the left of Custom.',
          preferFallbackText: true,
        },
      )],
    };
  },
  open_custom_node: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'customNodeId', { kind: 'selector', selectorKey: 'node-canvas' }),
    'click',
    '点击黄色高亮的自定义节点，然后看右侧下方是否出现它的彩色积木编辑区。',
    { fallbackEn: 'Click the yellow-highlighted Custom node, then check that its colorful block editor appears in the lower-right area.', preferFallbackText: true }
  )] }),
  add_when_enter: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    { kind: 'selector', selectorKey: 'node-blockly-editor' },
    'drag',
    '把蓝色高亮、表面写着“当进入当前节点时”的积木，拖到黄色高亮的右侧空白编辑区。',
    {
      fromTarget: { kind: 'block-type', blockType: 'node_when_enter' },
      fallbackEn: 'Drag the blue-highlighted block labeled "When entering this node" into the yellow-highlighted empty editor on the right.',
      preferFallbackText: true,
    }
  )] }),
  add_set_position: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'whenEnterBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
    'drag',
    '\u628a\u84dd\u8272\u9ad8\u4eae\u3001\u8868\u9762\u5199\u7740\u201c\u8bbe\u7f6e\u5bf9\u8c61\u2026\u4f4d\u7f6e X\u2026Y\u2026Z\u2026\u201d\u7684\u79ef\u6728\uff0c\u62d6\u8fdb\u9ec4\u8272\u9ad8\u4eae\u7684\u201c\u5f53\u8fdb\u5165\u5f53\u524d\u8282\u70b9\u65f6\u201d\u79ef\u6728\u91cc\uff0c\u76f4\u5230\u81ea\u52a8\u54ac\u5408\u3002',
    {
      fromTarget: { kind: 'block-type', blockType: 'object_set_position' },
      fallbackEn: 'Drag the blue-highlighted "Set object ... Position X ... Y ... Z ..." block into the yellow-highlighted "When entering this node" block until they snap together.',
      preferFallbackText: true,
    }
  )] }),
  set_position_model: (source) => {
    const modelName = String(source.bindings?.modelActorName || '');
    return { panelId: 'NodeGraphPanel', steps: [tutorialStep(
      source,
      targetFromBinding(source.bindings, 'setPositionBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
      'input',
      modelName
        ? `\u5728\u9ec4\u8272\u9ad8\u4eae\u79ef\u6728\u6700\u4e0a\u65b9\u7684\u5bf9\u8c61\u540d\u79f0\u6846\u4e2d\uff0c\u5b8c\u6574\u8f93\u5165\u201c${modelName}\u201d\u3002`
        : '\u5728\u9ec4\u8272\u9ad8\u4eae\u79ef\u6728\u6700\u4e0a\u65b9\u7684\u5bf9\u8c61\u540d\u79f0\u6846\u4e2d\uff0c\u5b8c\u6574\u8f93\u5165\u7b2c\u4e8c\u7ae0\u6559\u7a0b\u6a21\u578b\u540d\u79f0\u3002',
      {
        fallbackEn: modelName
          ? `Enter the full model name "${modelName}" in the top name field of the yellow-highlighted block.`
          : 'Enter the full Chapter 2 tutorial model name in the top name field of the yellow-highlighted block.',
        preferFallbackText: true,
      }
    )] };
  },
  set_start_x: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'setPositionBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
    'input',
    '\u5728\u9ec4\u8272\u9ad8\u4eae\u7684\u4f4d\u7f6e\u79ef\u6728\u4e2d\uff0c\u628a\u201c\u4f4d\u7f6e X\u201d\u540e\u9762\u7684\u6570\u5b57\u6539\u4e3a -3\uff0cY \u548c Z \u4fdd\u6301 0\u3002',
    { fallbackEn: 'In the yellow-highlighted position block, change Position X to -3 and keep Y and Z at 0.', preferFallbackText: true }
  )] }),
  add_while_active: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    { kind: 'selector', selectorKey: 'node-blockly-editor' },
    'drag',
    '\u628a\u84dd\u8272\u9ad8\u4eae\u3001\u8868\u9762\u5199\u7740\u201c\u5f53\u524d\u8282\u70b9\u6301\u7eed\u65f6\u201d\u7684\u79ef\u6728\uff0c\u62d6\u5230\u9ec4\u8272\u9ad8\u4eae\u7f16\u8f91\u533a\u7684\u53e6\u4e00\u5757\u7a7a\u767d\u4f4d\u7f6e\uff0c\u4e0d\u8981\u653e\u8fdb\u4e0a\u4e00\u5757\u4e8b\u4ef6\u79ef\u6728\u91cc\u3002',
    {
      fromTarget: { kind: 'block-type', blockType: 'node_while_active' },
      fallbackEn: 'Drag the blue-highlighted "While this node is active" block to a separate empty spot in the yellow-highlighted editor. Do not place it inside the previous event block.',
      preferFallbackText: true,
    }
  )] }),
  add_move_direction: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'whileActiveBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
    'drag',
    '\u628a\u84dd\u8272\u9ad8\u4eae\u3001\u8868\u9762\u5199\u7740\u201c\u8ba9\u5bf9\u8c61\u2026\u6301\u7eed\u79fb\u52a8\uff0c\u65b9\u5411\u2026\u901f\u5ea6\u2026\u201d\u7684\u79ef\u6728\uff0c\u62d6\u8fdb\u9ec4\u8272\u9ad8\u4eae\u7684\u201c\u5f53\u524d\u8282\u70b9\u6301\u7eed\u65f6\u201d\u79ef\u6728\u91cc\uff0c\u76f4\u5230\u81ea\u52a8\u54ac\u5408\u3002',
    {
      fromTarget: { kind: 'block-type', blockType: 'object_move_direction' },
      fallbackEn: 'Drag the blue-highlighted "Move object ... continuously, Direction ... Speed ..." block into the yellow-highlighted "While this node is active" block until it snaps in.',
      preferFallbackText: true,
    }
  )] }),
  set_move_model: (source) => {
    const modelName = String(source.bindings?.modelActorName || '');
    return { panelId: 'NodeGraphPanel', steps: [tutorialStep(
      source,
      targetFromBinding(source.bindings, 'moveDirectionBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
      'input',
      modelName
        ? `\u5728\u9ec4\u8272\u9ad8\u4eae\u7684\u6301\u7eed\u79fb\u52a8\u79ef\u6728\u7b2c\u4e00\u884c\uff0c\u5b8c\u6574\u8f93\u5165\u6a21\u578b\u540d\u79f0\u201c${modelName}\u201d\u3002`
        : '\u5728\u9ec4\u8272\u9ad8\u4eae\u7684\u6301\u7eed\u79fb\u52a8\u79ef\u6728\u7b2c\u4e00\u884c\uff0c\u5b8c\u6574\u8f93\u5165\u7b2c\u4e8c\u7ae0\u6559\u7a0b\u6a21\u578b\u540d\u79f0\u3002',
      {
        fallbackEn: modelName
          ? `Enter the full model name "${modelName}" on the first line of the yellow-highlighted movement block.`
          : 'Enter the full Chapter 2 tutorial model name on the first line of the yellow-highlighted movement block.',
        preferFallbackText: true,
      }
    )] };
  },
  set_move_direction: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'moveDirectionBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
    'input',
    '\u5728\u9ec4\u8272\u9ad8\u4eae\u79ef\u6728\u7684\u201c\u65b9\u5411\u201d\u4e0b\u62c9\u6846\u4e2d\uff0c\u9009\u62e9\u201c\u5411\u53f3\u201d\u3002',
    { fallbackEn: 'Choose Right from the Direction list in the yellow-highlighted movement block.', preferFallbackText: true }
  )] }),
  set_move_speed: (source) => ({ panelId: 'NodeGraphPanel', steps: [tutorialStep(
    source,
    targetFromBinding(source.bindings, 'moveDirectionBlockId', { kind: 'selector', selectorKey: 'node-blockly-editor' }),
    'input',
    '\u5728\u9ec4\u8272\u9ad8\u4eae\u79ef\u6728\u4e2d\uff0c\u628a\u201c\u901f\u5ea6\u201d\u540e\u9762\u7684\u6570\u5b57\u6539\u4e3a 2\u3002\u6a21\u578b\u540d\u79f0\u8981\u6b63\u786e\uff0c\u65b9\u5411\u8981\u4fdd\u6301\u201c\u5411\u53f3\u201d\u3002',
    { fallbackEn: 'Set Speed to 2 in the yellow-highlighted block. Keep the correct model name and Direction set to Right.', preferFallbackText: true }
  )] }),
  run_node_graph: basicGuidance(
    'NodeGraphPanel',
    { kind: 'selector', selectorKey: 'node-run' },
    'click',
    '\u70b9\u51fb\u8282\u70b9\u7a97\u53e3\u4e0a\u65b9\u7684\u201c\u8fd0\u884c\u201d\u6309\u94ae\u4e00\u6b21\u3002\u70b9\u51fb\u540e\uff0c\u6a21\u578b\u4f1a\u5148\u8df3\u5230 X=-3\uff0c\u7136\u540e\u4ee5\u6bcf\u79d2 2 \u4e2a\u5355\u4f4d\u7684\u901f\u5ea6\u6301\u7eed\u5411\u53f3\u79fb\u52a8\u3002\u4efb\u52a1\u5728\u4f60\u70b9\u51fb\u65f6\u7acb\u5373\u5b8c\u6210\u3002',
    { fallbackEn: 'Click Run once. The model will jump to X=-3 and then keep moving right at two units per second. The task completes immediately when you click.' },
  ),
  start_preview: basicGuidance(
    'MainPage',
    { kind: 'selector', selectorKey: 'preview-start' },
    'click',
    '点击“开始预览”，等到预览画面真正启动。',
    { fallbackEn: 'Click Start Preview and wait until the preview is visibly running.' },
  ),
  stop_preview: basicGuidance(
    'MainPage',
    { kind: 'selector', selectorKey: 'preview-stop' },
    'click',
    '点击“结束预览”，等待预览完全停止且场景恢复。',
    { fallbackEn: 'Click End Preview and wait until preview stops completely and the scene is restored.' },
  ),
  focus_ai_composer: basicGuidance(
    'MainPage',
    { kind: 'selector', selectorKey: 'cabbage-chat-input' },
    'click',
    '\u70b9\u51fb\u53f3\u4fa7\u201cAI \u521b\u4f5c\u52a9\u624b\u201d\u6700\u4e0b\u65b9\u7684\u5927\u8f93\u5165\u6846\u3002\u63d0\u793a\u8bcd\u5c31\u662f\u4f60\u5199\u7ed9 AI \u7684\u5177\u4f53\u8981\u6c42\u3002',
    {
      fallbackEn: 'Click the large input box at the bottom of AI Creative Assistant. A prompt is the specific request you write for the AI.',
      preferFallbackText: true,
    },
  ),
  ask_ai_question: (source) => ({ panelId: 'MainPage', steps: [
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-input' },
      'input',
      '\u8f93\u5165\u4e00\u53e5\u53ea\u8bf7 AI \u89e3\u91ca\u3001\u4e0d\u4fee\u6539\u7684\u95ee\u9898\u3002\u793a\u4f8b\uff1a\u201c\u8bf7\u544a\u8bc9\u6211\u7b49\u5f85 2 \u79d2\u4f1a\u5728\u4ec0\u4e48\u65f6\u5019\u6267\u884c\uff0c\u53ea\u89e3\u91ca\uff0c\u4e0d\u8981\u4fee\u6539\u8282\u70b9\u56fe\u3002\u201d',
      { fallbackEn: 'Enter a question that asks for an explanation only. Example: "Explain why the tutorial model starts at X=-3 and then keeps moving right. Explain only; do not modify the node graph."', preferFallbackText: true },
    ),
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-send' },
      'click',
      '\u70b9\u51fb\u201c\u53d1\u9001\u201d\uff0c\u7136\u540e\u7b49\u5f85\u201cAI \u521b\u4f5c\u52a9\u624b\u201d\u771f\u6b63\u8fd4\u56de\u56de\u7b54\u3002',
      { fallbackEn: 'Click Send, then wait until AI Creative Assistant returns a real answer.', preferFallbackText: true },
    ),
  ] }),
  modify_with_ai: (source) => ({ panelId: 'MainPage', steps: [
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-input' },
      'input',
      '\u8f93\u5165\u4e00\u53e5\u5177\u4f53\u4fee\u6539\u8981\u6c42\u3002\u793a\u4f8b\uff1a\u201c\u8bf7\u628a\u6559\u7a0b\u6a21\u578b\u6301\u7eed\u5411\u53f3\u79fb\u52a8\u7684\u901f\u5ea6\u4ece 2 \u6539\u4e3a 4\uff0c\u53ea\u4fee\u6539\u901f\u5ea6\uff0c\u5176\u4ed6\u5185\u5bb9\u4fdd\u6301\u4e0d\u53d8\u3002\u201d',
      { fallbackEn: 'Enter a specific edit request. Example: "Change the tutorial model\'s continuous rightward movement speed from 2 to 4. Change only the speed and keep everything else unchanged."', preferFallbackText: true },
    ),
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-send' },
      'click',
      '\u70b9\u51fb\u201c\u53d1\u9001\u201d\uff0c\u7b49\u5f85 AI \u771f\u6b63\u5b8c\u6210\u5e76\u4fdd\u5b58\u4fee\u6539\u3002',
      { fallbackEn: 'Click Send and wait until the AI actually applies and saves the edit.', preferFallbackText: true },
    ),
  ] }),
  generate_with_ai: (source) => ({ panelId: 'MainPage', steps: [
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-input' },
      'input',
      '\u8f93\u5165\u4e00\u53e5\u201c\u589e\u52a0\u65b0\u5185\u5bb9\u201d\u7684\u8981\u6c42\u3002\u793a\u4f8b\uff1a\u201c\u8bf7\u589e\u52a0\u4e00\u4e2a\u7ed3\u675f\u8282\u70b9\uff0c\u628a\u5f53\u524d\u81ea\u5b9a\u4e49\u8282\u70b9\u8fde\u63a5\u5230\u5b83\uff0c\u4fdd\u7559\u5df2\u6709\u8282\u70b9\u548c\u79ef\u6728\u3002\u201d',
      { fallbackEn: 'Enter an add request. Example: "Add an End node, connect the current Custom node to it, and keep all existing nodes and blocks."', preferFallbackText: true },
    ),
    tutorialStep(
      source,
      { kind: 'selector', selectorKey: 'cabbage-chat-send' },
      'click',
      '\u70b9\u51fb\u201c\u53d1\u9001\u201d\uff0c\u7b49\u5f85 AI \u589e\u52a0\u5e76\u4fdd\u5b58\u65b0\u903b\u8f91\u3002\u5b8c\u6210\u540e\u6559\u7a0b\u4f1a\u7ed3\u675f\uff0c\u4f60\u521b\u5efa\u7684\u6a21\u578b\u3001\u8282\u70b9\u548c\u79ef\u6728\u90fd\u4f1a\u4fdd\u7559\u3002',
      { fallbackEn: 'Click Send and wait until the AI adds and saves the new logic. When the tutorial ends, your model, nodes, and blocks all remain in the project.', preferFallbackText: true },
    ),
  ] }),
});

const ISSUE_GUIDANCE = Object.freeze({
  missing_actor_target: { selectorKey: 'node-blockly-editor', action: 'connect', text: '定位到对应操作积木，把“对象[]”积木接到对象输入口并选择场景中的目标物体。' },
  actor_target_not_found: { selectorKey: 'node-blockly-editor', action: 'click', text: '定位到对象引用积木，改为当前场景中真实存在的物体。' },
  start_node_count: { selectorKey: 'node-canvas', action: 'focus', text: '检查节点编辑区，只保留一个开始节点，并让它连接到首个逻辑节点。' },
  invalid_edge_endpoint: { selectorKey: 'node-canvas', action: 'connect', text: '重新连接这条连线，确保起点和终点都连接到有效节点端口。' },
  invalid_visible_condition_count: { selectorKey: 'node-transition-condition', action: 'connect', text: '选中对应连线，只保留一个完整的条件表达式。' },
  non_boolean_condition: { selectorKey: 'node-transition-condition', action: 'connect', text: '把连线条件改为返回 Boolean 的判断积木。' },
  unknown_block_type: { selectorKey: 'node-blockly-editor', action: 'click', text: '删除当前不支持的积木，并从左侧工具箱换成已有积木。' },
  missing_required_input: { selectorKey: 'node-blockly-editor', action: 'connect', text: '给积木缺失的关键输入口连接匹配类型的积木。' },
});

const CHAT_GUIDANCE = Object.freeze({
  connect_object_reference: ISSUE_GUIDANCE.missing_actor_target,
  select_existing_object: ISSUE_GUIDANCE.actor_target_not_found,
  create_node: LEGACY_TUTORIAL_GUIDANCE['tutorial.create_node'].steps[0],
  move_node: LEGACY_TUTORIAL_GUIDANCE['tutorial.move_node'].steps[0],
  connect_nodes: LEGACY_TUTORIAL_GUIDANCE['tutorial.connect_nodes'].steps[0],
  drag_block: LEGACY_TUTORIAL_GUIDANCE['tutorial.drag_block'].steps[0],
  edit_block_parameter: LEGACY_TUTORIAL_GUIDANCE['tutorial.edit_block_parameter'].steps[0],
  set_transition_condition: LEGACY_TUTORIAL_GUIDANCE['tutorial.set_transition_condition'].steps[0],
  run_node_graph: LEGACY_TUTORIAL_GUIDANCE['tutorial.run_node_graph'].steps[0],
  import_model: LEGACY_TUTORIAL_GUIDANCE['tutorial.import_model'].steps[0],
  transform_model: LEGACY_TUTORIAL_GUIDANCE['tutorial.transform_model'].steps[0],
  adjust_lighting: LEGACY_TUTORIAL_GUIDANCE['tutorial.adjust_lighting'].steps[0],
  adjust_physics: LEGACY_TUTORIAL_GUIDANCE['tutorial.adjust_physics'].steps[0],
});

function guidanceForTask(source = {}) {
  if (source.type === 'goal') {
    const intent = String(source.guidanceIntent || '');
    const template = CHAT_GUIDANCE[intent];
    if (!template) return null;
    const panelId = intent === 'adjust_lighting'
      ? 'MainPage'
      : intent === 'import_model'
        ? 'SceneTools'
      : ['transform_model', 'adjust_physics'].includes(intent)
        ? 'Object'
        : 'NodeGraphPanel';
    return { panelId, steps: [{ ...template }] };
  }
  const intent = String(source.guidanceIntent || '');
  const basicsFactory = BASICS_TUTORIAL_GUIDANCE[intent];
  if (basicsFactory) return basicsFactory(source);
  const taskKey = String(source.taskKey || source.issueKey || '');
  const tutorial = LEGACY_TUTORIAL_GUIDANCE[taskKey];
  if (tutorial) return { ...tutorial, steps: tutorial.steps.map((step) => ({ ...step })) };

  const issue = ISSUE_GUIDANCE[String(source.code || '')] || ISSUE_GUIDANCE.missing_required_input;
  const preciseTarget = source.blockId
    ? { kind: 'block', blockId: String(source.blockId) }
    : source.nodeId
      ? { kind: 'node', nodeId: String(source.nodeId) }
      : source.edgeId
        ? { kind: 'edge', edgeId: String(source.edgeId) }
        : { kind: 'selector', selectorKey: issue.selectorKey };
  return {
    panelId: 'NodeGraphPanel',
    steps: [{ target: preciseTarget, action: issue.action, text: issue.text }],
  };
}

function panelIdForTarget(target = {}) {
  const selectorKey = String(target.selectorKey || '');
  if (['main-viewport', 'scene-shortcut', 'node-shortcut', 'scene-lighting', 'scene-light-x', 'preview-start', 'preview-stop'].includes(selectorKey)) return 'MainPage';
  if (selectorKey.startsWith('scene-')) return 'SceneTools';
  if (selectorKey.startsWith('settings-')) return 'EditorSettings';
  if (selectorKey.startsWith('object-')) return 'Object';
  if (selectorKey.startsWith('node-')) return 'NodeGraphPanel';
  if (target.kind === 'actor') return 'SceneTools';
  if (['node', 'block', 'block-type', 'edge', 'port'].includes(String(target.kind || ''))) {
    return 'NodeGraphPanel';
  }
  return '';
}

function normalizeGuidanceStep(step = {}) {
  return {
    ...step,
    target: step?.target ? { ...step.target } : {},
    ...(step?.fromTarget ? { fromTarget: { ...step.fromTarget } } : {}),
  };
}

function normalizeGuidance(source, sourceType, resolved) {
  const steps = (resolved?.steps || []).map(normalizeGuidanceStep);
  if (!steps.length) return null;
  const inferredPanelId = steps
    .map((step) => panelIdForTarget(step.target) || panelIdForTarget(step.fromTarget))
    .find(Boolean);
  const panelId = inferredPanelId || String(resolved?.panelId || source?.panelId || '');
  if (!PANEL_ZONES[panelId]) return null;
  return {
    guidanceId: String(source.guidanceId || source.taskKey || source.issueKey || source.tipKey || source.id || `guidance_${Date.now()}`),
    sourceType,
    title: String(source.title || '\u64cd\u4f5c\u5c55\u793a'),
    panelId,
    steps,
  };
}

export const guidanceRegistry = {
  resolve(source = {}) {
    const sourceType = String(source.sourceType || source.type || 'node-issue');
    if (source?.steps && Array.isArray(source.steps)) {
      // Persisted worlds may contain legacy panelId/dockZone values. Re-infer the
      // current panel from the target so old data cannot reopen the old layout.
      return normalizeGuidance(source, sourceType, source);
    }
    let resolved;
    if (sourceType === 'chat') {
      const template = CHAT_GUIDANCE[String(source.guidanceIntent || '')];
      if (!template) return null;
      const panelId = String(source.panelId || (
        source.guidanceIntent === 'adjust_lighting' ? 'MainPage'
          : source.guidanceIntent === 'import_model' ? 'SceneTools'
          : ['transform_model', 'adjust_physics'].includes(source.guidanceIntent) ? 'Object'
            : 'NodeGraphPanel'
      ));
      resolved = { panelId, steps: [{ ...template }] };
    } else if (sourceType === 'optimization-tip') {
      resolved = { panelId: 'NodeGraphPanel', steps: [stepFor('node-canvas', 'focus', source.message || '\u67e5\u770b\u5f53\u524d\u8282\u70b9\u56fe\u4e2d\u53ef\u4ee5\u4f18\u5316\u7684\u63a7\u5236\u6d41\u3002')] };
    } else {
      resolved = guidanceForTask(source);
    }
    return normalizeGuidance(source, sourceType, resolved);
  },
};

async function showStep(index) {
  if (!state.guidance?.steps?.length) return;
  state.stepIndex = Math.max(0, Math.min(index, state.guidance.steps.length - 1));
  exactTargetSeen = false;
  exactTargetMissingTicks = 0;
  exactTargetPrepareTicks = 0;
  fromTargetPrepareTicks = 0;
  const step = currentStep();
  prepareGuidanceTarget(step?.target || {});
  await wait(80);
  revealTarget(step?.target || {});
  if (step?.fromTarget) {
    prepareGuidanceTarget(step.fromTarget);
    await wait(120);
    revealTarget(step.fromTarget, { allowFallback: false });
  } else {
    await wait(120);
  }
  refreshRects();
}

function handleProjectChanged() {
  if (state.active || state.preparing) void stop({ restorePanelState: false });
}

function handlePageUnload() {
  stopRectTracking();
  restoreState = null;
  state.active = false;
  state.preparing = false;
}

function attachLifecycleGuards() {
  if (lifecycleGuardsAttached) return;
  lifecycleGuardsAttached = true;
  window.addEventListener('corona-active-project-changed', handleProjectChanged);
  window.addEventListener('beforeunload', handlePageUnload);
}

function detachLifecycleGuards() {
  if (!lifecycleGuardsAttached) return;
  lifecycleGuardsAttached = false;
  window.removeEventListener('corona-active-project-changed', handleProjectChanged);
  window.removeEventListener('beforeunload', handlePageUnload);
}

async function start(source) {
  const guidance = guidanceRegistry.resolve(source);
  if (!guidance) return false;
  if (state.active || state.preparing) await stop();
  const token = ++lifecycleToken;
  state.preparing = true;
  attachLifecycleGuards();
  const panelReady = await ensurePanel(guidance.panelId, guidance.steps[0]?.target?.selectorKey || '');
  if (token !== lifecycleToken || !panelReady) {
    if (token === lifecycleToken) await stop({ restorePanelState: false });
    return false;
  }
  state.guidance = guidance;
  state.stepIndex = 0;
  state.active = true;
  state.preparing = false;
  await showStep(0);
  if (token !== lifecycleToken) return false;
  startRectTracking();
  return true;
}

async function stop({ restorePanelState = true } = {}) {
  ++lifecycleToken;
  stopRectTracking();
  state.active = false;
  state.guidance = null;
  state.stepIndex = 0;
  state.targetRect = null;
  state.fromRect = null;
  state.preparing = false;
  exactTargetSeen = false;
  exactTargetMissingTicks = 0;
  exactTargetPrepareTicks = 0;
  fromTargetPrepareTicks = 0;
  detachLifecycleGuards();
  if (restorePanelState) await restorePanel();
  else restoreState = null;
}

function next() {
  if (!state.active) return;
  if (state.stepIndex >= state.guidance.steps.length - 1) {
    void stop();
    return;
  }
  void showStep(state.stepIndex + 1);
}

function previous() {
  if (!state.active || state.stepIndex <= 0) return;
  void showStep(state.stepIndex - 1);
}

export const guidanceService = {
  state,
  start,
  next,
  previous,
  stop,
  refreshTarget: refreshRects,
};
