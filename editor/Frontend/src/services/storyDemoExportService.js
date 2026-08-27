import { editorApi } from '@/api/editorApi.js';
import { buildPlayableDemoManifest } from '@/utils/storyDemo.js';
export function buildStoryDemoExportManifest(document) { return buildPlayableDemoManifest(document); }
export async function exportPlayableStoryDemo(document, targetDirectory, sourceProjectPath = '') {
  const manifest = buildPlayableDemoManifest(document);
  const exporter = editorApi.project?.exportPlayableStoryDemo;
  if (typeof exporter !== 'function') throw new Error('当前运行时尚未提供独立 Demo 打包接口；创作内容已保存在本机，可继续编辑后再导出。');
  const source = String(sourceProjectPath || document?.projectKey || '').trim();
  const payload = { targetDirectory, manifest, document };
  if (source && source.toLowerCase() !== 'active-project') payload.sourceProjectPath = source;
  return exporter(payload);
}
