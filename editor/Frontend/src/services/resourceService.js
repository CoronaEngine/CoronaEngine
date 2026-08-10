import { editorApi } from '../api/editorApi.js';

const CURRENT_CALLER = 'SceneBar';
const RESOURCE_SEARCH_ENABLED = false;

const resourceSearchDisabled = () =>
  Promise.resolve({
    success: true,
    data: {
      status: 'disabled',
      code: 'resource_search_disabled',
      message: 'ResourceSearch is disabled',
      items: [],
      total: 0,
    },
  });

/** Compatibility facade for the resource-search panel. */
export const resourceService = {
  prepareIndex: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.prepareIndex(CURRENT_CALLER)
      : resourceSearchDisabled(),
  fuzzySearch: (query, topK = 20, typeFilter = null) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.fuzzySearch(query, topK, typeFilter, CURRENT_CALLER)
      : resourceSearchDisabled(),
  imageSearch: (imageB64, topK = 20, threshold = 10) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.imageSearch(imageB64, topK, threshold, CURRENT_CALLER)
      : resourceSearchDisabled(),
  listTypes: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.listTypes(CURRENT_CALLER)
      : resourceSearchDisabled(),
  rebuildIndex: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.rebuildIndex(CURRENT_CALLER)
      : resourceSearchDisabled(),
  getStats: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.getStats(CURRENT_CALLER)
      : resourceSearchDisabled(),
  markIndexDirty: (reason = 'frontend') =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.markIndexDirty(reason, CURRENT_CALLER)
      : resourceSearchDisabled(),
  focusActor: (sceneName, actorName) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.focusActor(sceneName, actorName, CURRENT_CALLER)
      : resourceSearchDisabled(),
};
