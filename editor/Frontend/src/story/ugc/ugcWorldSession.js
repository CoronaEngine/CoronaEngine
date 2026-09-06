/**
 * UGC 世界会话：隔离主世界运行状态，并注入材料和世界碎片资源。
 */
export function createUgcWorldSession() {
  let state = 'created';
  let resources = { materials: [], fragments: [] };
  let snapshot = null;

  return {
    create() {
      state = 'created';
      return this;
    },

    loadResources(materials = [], fragments = []) {
      resources = {
        materials: structuredClone(materials),
        fragments: structuredClone(fragments),
      };
      return resources;
    },

    enter(mainWorldState) {
      snapshot = structuredClone(mainWorldState ?? null);
      state = 'entered';
      return { state, resources: structuredClone(resources) };
    },

    exit() {
      state = 'exited';
      return snapshot;
    },

    getState() {
      return { state, resources: structuredClone(resources) };
    },

    dispose() {
      state = 'disposed';
      snapshot = null;
      resources = { materials: [], fragments: [] };
    },
  };
}
