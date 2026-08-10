/** Compatibility facade for the LAN chat and agent room APIs. */

import { editorApi } from '../api/editorApi.js';

const unwrap = (res) => (res && res.data !== undefined ? res.data : res);

export const lanChatService = {
  startRoom: (payload) => editorApi.lanChat.startRoom(payload).then(unwrap),
  startLocalRoom: (payload) => editorApi.lanChat.startLocalRoom(payload).then(unwrap),
  stopRoom: () => editorApi.lanChat.stopRoom().then(unwrap),
  stopLocalRoom: () => editorApi.lanChat.stopLocalRoom().then(unwrap),
  joinRoom: (payload) => editorApi.lanChat.joinRoom(payload).then(unwrap),
  getHistory: () => editorApi.lanChat.getHistory().then(unwrap),
  listHistoryRooms: () => editorApi.lanChat.listHistoryRooms().then(unwrap),
  loadHistoryRoom: (room) => editorApi.lanChat.loadHistoryRoom(room).then(unwrap),
  leaveRoom: () => editorApi.lanChat.leaveRoom().then(unwrap),
  sendMessage: (text, options = {}) =>
    editorApi.lanChat.sendMessage(text, options).then(unwrap),
  sendAgentReply: (payload) => editorApi.lanChat.sendAgentReply(payload).then(unwrap),
  sendSystemMessage: (payload) => editorApi.lanChat.sendSystemMessage(payload).then(unwrap),
  sendSystemMessageToHost: (payload) =>
    editorApi.lanChat.sendSystemMessageToHost(payload).then(unwrap),
  sendSystemMessageToUser: (payload) =>
    editorApi.lanChat.sendSystemMessageToUser(payload).then(unwrap),
  pollAgentTrigger: () => editorApi.lanChat.pollAgentTrigger().then(unwrap),
  pollCoordinatorSyncMessage: () =>
    editorApi.lanChat.pollCoordinatorSyncMessage().then(unwrap),
  pollRoomEvent: () => editorApi.lanChat.pollRoomEvent().then(unwrap),
  pollSyncEvent: () => editorApi.lanChat.pollSyncEvent().then(unwrap),
  getLocalIp: () => editorApi.lanChat.getLocalIp().then(unwrap),
  addAgent: (payload) => editorApi.lanChat.addAgent(payload).then(unwrap),
  removeAgent: (agentId) => editorApi.lanChat.removeAgent(agentId).then(unwrap),
  listAgents: () => editorApi.lanChat.listAgents().then(unwrap),
};
