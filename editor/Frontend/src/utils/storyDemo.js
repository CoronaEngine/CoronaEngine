import { STORY_DEMO_SLOT_TYPES, validateStoryCoreSlot } from '../config/storyDemo.js';
export function normalizeDemoActor(actor = {}) {
  return {
    id: String(actor.id || Date.now()),
    name: String(actor.name || ''),
    asset: String(actor.asset || ''),
    position: Array.isArray(actor.position) ? actor.position.slice(0, 3).map(Number) : [0, 0, 0],
    rotation: Array.isArray(actor.rotation) ? actor.rotation.slice(0, 3).map(Number) : [0, 0, 0],
    scale: Array.isArray(actor.scale) ? actor.scale.slice(0, 3).map(Number) : [1, 1, 1],
    componentType: String(actor.componentType || 'object'),
    generatedBySlot: actor.generatedBySlot ? String(actor.generatedBySlot) : '',
  };
}
export function addDemoActor(document, actor) { const next = { ...document, actors: [...(Array.isArray(document?.actors) ? document.actors : []), normalizeDemoActor(actor)] }; return next; }
export function removeDemoActor(document, actorId) { return { ...document, actors: (document?.actors || []).filter((actor) => String(actor.id) !== String(actorId)) }; }
export function setDemoCoreSlot(document, slotType, item) { if (!STORY_DEMO_SLOT_TYPES.includes(slotType) || !validateStoryCoreSlot(slotType, item)) return { document, changed: false }; return { document: { ...document, slots: { ...(document.slots || {}), [slotType]: item }, updatedAt: Date.now() }, changed: true }; }
export function buildPlayableDemoManifest(document = {}) { return { format: 'corona-story-demo', version: 1, demoName: String(document.name || '未命名 Demo'), worldBallId: String(document.worldBallId || ''), sceneName: String(document.sceneName || ''), readOnly: true, coreSlots: document.slots || {}, actors: Array.isArray(document.actors) ? document.actors : [], generatedAt: new Date().toISOString() }; }
