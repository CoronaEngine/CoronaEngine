export const resolveOpenedProjectScene = (payload = {}) => {
  const route = payload?.scene?.path ?? payload?.scene?.route ?? payload?.scene_path;
  return typeof route === 'string' ? route.trim() : '';
};

export const resolveSceneTabs = (initData = {}, fallbackRoute = '') => {
  const scenes = Array.isArray(initData?.scenes) ? initData.scenes : [];
  const tabs = scenes
    .filter((scene) => typeof scene?.path === 'string' && scene.path.trim())
    .map((scene) => ({
      name: scene.name || scene.path,
      id: scene.path,
    }));

  if (tabs.length) {
    const requestedIndex = Number(initData?.active_index);
    const activeIndex = Number.isFinite(requestedIndex)
      ? Math.min(Math.max(requestedIndex, 0), tabs.length - 1)
      : 0;
    return { tabs, activeIndex };
  }

  const fallbackId = typeof initData?.path === 'string' && initData.path.trim()
    ? initData.path
    : fallbackRoute;
  return {
    tabs: [{ name: initData?.name || fallbackId, id: fallbackId }],
    activeIndex: 0,
  };
};
