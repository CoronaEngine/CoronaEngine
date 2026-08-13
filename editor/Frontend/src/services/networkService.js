/** Compatibility facade for network sessions, locks, and sync queues. */

import { editorApi } from '../api/editorApi.js';

const unwrap = (res) => (res && res.data !== undefined ? res.data : res);

export const networkService = {
  startSession: (instanceName, projectId, port = 27960, role = 'host') =>
    editorApi.network.startSession(instanceName, projectId, port, role).then(unwrap),
  stopSession: () => editorApi.network.stopSession().then(unwrap),
  getPeerCount: () => editorApi.network.getPeerCount().then(unwrap),
  getSessionInfo: () => editorApi.network.getSessionInfo().then(unwrap),
  getDiscoveredPeers: () => editorApi.network.getDiscoveredPeers().then(unwrap),
  clearDiscoveredPeers: () => editorApi.network.clearDiscoveredPeers().then(unwrap),
  searchLan: () => editorApi.network.searchLan().then(unwrap),
  connectToPeer: (ip, port, peerName) =>
    editorApi.network.connectToPeer(ip, port, peerName).then(unwrap),
  lockObject: (objectId, userId, operation = 'modify') =>
    editorApi.network.lockObject(objectId, userId, operation).then(unwrap),
  unlockObject: (objectId, userId) =>
    editorApi.network.unlockObject(objectId, userId).then(unwrap),
  getLockOwner: (objectId) => editorApi.network.getLockOwner(objectId).then(unwrap),
  broadcastIntent: (userId, tooltip, position, status = 'placing_object') =>
    editorApi.network.broadcastIntent(userId, tooltip, position, status).then(unwrap),
  checkPreviewCollision: (userId, position, delta = 0.5) =>
    editorApi.network.checkPreviewCollision(userId, position, delta).then(unwrap),
  setProjectRoot: (projectRoot) => editorApi.network.setProjectRoot(projectRoot).then(unwrap),
  broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>
    editorApi.network.broadcastActorCreate(actorGuid, sceneName, modelPath, actorData).then(unwrap),
  broadcastActorTransform: (actorGuid, sceneName, actorData) =>
    editorApi.network.broadcastActorTransform(actorGuid, sceneName, actorData).then(unwrap),
  broadcastActorDelete: (actorGuid, sceneName, actorName) =>
    editorApi.network.broadcastActorDelete(actorGuid, sceneName, actorName).then(unwrap),
  requestSceneSnapshot: (sceneName) =>
    editorApi.network.requestSceneSnapshot(sceneName).then(unwrap),
  broadcastSceneSnapshot: (sceneName, snapshot) =>
    editorApi.network.broadcastSceneSnapshot(sceneName, snapshot).then(unwrap),
  broadcastActorStateUpdate: (actorGuid, sceneName, actorData) =>
    editorApi.network.broadcastActorStateUpdate(actorGuid, sceneName, actorData).then(unwrap),
  pollPendingActorCreate: () => editorApi.network.pollPendingActorCreate().then(unwrap),
  pollPendingActorTransform: () => editorApi.network.pollPendingActorTransform().then(unwrap),
  pollPendingActorDelete: () => editorApi.network.pollPendingActorDelete().then(unwrap),
  pollPendingSceneSnapshotRequest: () =>
    editorApi.network.pollPendingSceneSnapshotRequest().then(unwrap),
  pollPendingSceneSnapshot: () => editorApi.network.pollPendingSceneSnapshot().then(unwrap),
  pollPendingActorStateUpdate: () =>
    editorApi.network.pollPendingActorStateUpdate().then(unwrap),
  setSyncPaused: (paused) => editorApi.network.setSyncPaused(paused).then(unwrap),
  registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>
    editorApi.network.registerActorIdentity(actorGuid, actorHandle, locallyOwned).then(unwrap),
  claimActorOwnership: (actorGuid) =>
    editorApi.network.claimActorOwnership(actorGuid).then(unwrap),
};
