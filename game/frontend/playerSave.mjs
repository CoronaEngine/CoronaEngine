import { unwrap } from './storyCharacters.mjs';

/** Acknowledged save; failed writes retain dirty state and can be retried. */
export function createPlayerSave({ api, sceneId, readPlayer, stopInput }) {
  let savedVersion = 0, inFlight = null;
  async function save() {
    stopInput();
    if (inFlight) await inFlight;
    const pose = readPlayer();
    if (!pose || pose.version === savedVersion) return;
    const operation = (async () => {
      const result = unwrap(await api.scene.setActorTransform(sceneId, pose.actorGuid, {
        position: [...pose.position], rotation: [...pose.rotation], persist: true,
      }));
      if (result?.status !== 'success') throw new Error(result?.message || '保存玩家位置失败');
      savedVersion = pose.version;
    })();
    inFlight = operation;
    try { await operation; } finally { if (inFlight === operation) inFlight = null; }
  }
  return { save };
}
