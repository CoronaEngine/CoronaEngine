/**
 * AI, node-graph review/generation, and Cabbage workflow facade.
 *
 * The public C++ contract remains owned by `src/api/editorApi.js`; this file
 * only adapts legacy response shapes and operation names used by panels.
 */

import { editorApi } from '../api/editorApi.js';

export const aiService = {
  sendMessageToAIStream: (payload) => editorApi.ai.sendMessageToAIStream(payload),
  readLocalFileAsBase64: (filePath) => editorApi.ai.readLocalFileAsBase64(filePath),
  generateHint: (elementType, context = {}) => editorApi.ai.generateHint(elementType, context),
  startNodeGraphReview: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.start',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  getNodeGraphReviewStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  chatAboutNodeGraph: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  startNodeGraphReviewChat: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.start',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  getNodeGraphReviewChatStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  cancelNodeGraphReviewChat: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.cancel',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  startNodeGraphGeneration: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.generate.start',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  getNodeGraphGenerationStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.generate.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  cancelNodeGraphGeneration: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.generate.cancel',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  loadCabbageContext: async (payload = {}) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.load',
      payload,
    });
    return response?.data ?? response;
  },
  recordCabbageEvent: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.record_event',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  updateCabbageTask: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.update_task',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  appendCabbageMessage: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.append_message',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  startCabbageGoalPlan: async (payload = {}) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.goal_plan.start',
      payload,
    });
    return response?.data ?? response;
  },
  getCabbageGoalPlanStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.goal_plan.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  startCabbageProfileScoreUpdate: async (payload = {}) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.profile.score.start',
      payload,
    });
    return response?.data ?? response;
  },
  getCabbageProfileScoreStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.profile.score.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
};

export const aiClient = {
  chatStream: (request) => editorApi.ai.chatStream(request),
  cancelRequest: (requestId) => editorApi.ai.cancelRequest(requestId),
  getRequestStatus: (requestId) => editorApi.ai.getRequestStatus(requestId),
};
