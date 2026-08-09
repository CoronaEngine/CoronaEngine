/**
 * Compatibility barrel for the historical bridge import path.
 *
 * The manifest-backed C++ contract and its transport now live in
 * `src/api/editorApi.js`. Production Vue code imports its API or service owner
 * directly; this path remains only for historical external host consumers.
 */

export * from '../api/editorApi.js';
export { sceneService } from '../compat/sceneService.js';
export { projectService } from '../compat/projectService.js';
export { appService } from '../compat/appService.js';
export { lanChatService } from '../services/lanChatService.js';
export { networkService } from '../services/networkService.js';
export { scriptingService } from '../compat/scriptingService.js';
export { aiService, aiClient } from '../services/aiService.js';
export { projectLauncherService } from '../services/projectLauncherService.js';
export { fileService } from '../compat/fileService.js';
export { projectSettingsService } from '../compat/projectSettingsService.js';
export { resourceService } from '../services/resourceService.js';
export { logService } from '../compat/logService.js';
