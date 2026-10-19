// Native project materialization is asynchronous: right after a project or scene
// is opened the native scene can legitimately report zero actors while its actors
// are still being created (observed with a 40 MB model, where the actor appeared
// ~10s after the project reported "opened").  Committing such a snapshot cleared
// the scene dock and nothing refreshed it afterwards, so the dock stayed empty.
//
// An empty snapshot is therefore provisional until the scene has shown actors or
// the settle window expired; the dock keeps polling inside that window.

export const SCENE_TREE_SETTLE_MS = 15000;
export const SCENE_TREE_SETTLE_POLL_MS = 500;

export const emptySceneTreeSettlement = () => ({
  sceneName: '',
  sawActors: false,
  deadline: 0,
});

// Resolves what the dock should do with one snapshot.
//
// `commit` replaces the visible actor rows, `rows` are the rows to commit (null
// while the snapshot is provisional), and `retryInMs` asks the caller to refresh
// again because the native scene may still be materializing.
export const resolveSceneTreeSnapshot = ({
  sceneName,
  actors,
  settlement = emptySceneTreeSettlement(),
  now = Date.now(),
} = {}) => {
  const rows = Array.isArray(actors) ? actors : [];
  const sceneChanged = settlement.sceneName !== sceneName;
  // The very first snapshot of this dock is not a switch: there are no rows from
  // another scene to drop, and an empty first snapshot during project open is the
  // exact case that used to leave the dock blank.
  const switchedFromAnotherScene = sceneChanged && settlement.sceneName !== '';
  const base = sceneChanged
    ? { sceneName, sawActors: false, deadline: now + SCENE_TREE_SETTLE_MS }
    : settlement;

  if (rows.length > 0) {
    return {
      commit: true,
      rows,
      retryInMs: 0,
      settlement: { ...base, sawActors: true },
    };
  }

  const provisional = !base.sawActors && now < base.deadline;
  return {
    // A scene switch must never keep showing the previous scene's actors, so an
    // empty snapshot for a new scene is always committed.
    commit: switchedFromAnotherScene || !provisional,
    rows,
    retryInMs: provisional ? SCENE_TREE_SETTLE_POLL_MS : 0,
    settlement: base,
  };
};
