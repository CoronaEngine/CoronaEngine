import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const frontendRoot = path.resolve(import.meta.dirname, '../../');
const sourceRoot = path.join(frontendRoot, 'src');

function sourceFiles(root) {
  const files = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const filePath = path.join(root, entry.name);
    if (entry.isDirectory()) files.push(...sourceFiles(filePath));
    else if (/\.(?:js|vue)$/.test(entry.name)) files.push(filePath);
  }
  return files;
}

test('Vue production code does not import the compatibility project service', () => {
  const violations = sourceFiles(sourceRoot)
    .filter((filePath) => !filePath.endsWith(`${path.sep}services${path.sep}projectService.js`))
    .filter((filePath) => fs.readFileSync(filePath, 'utf8').includes('services/projectService.js'));

  assert.deepEqual(violations, []);
});

test('Dock drag-region methods belong to appService', () => {
  const appService = fs.readFileSync(path.join(sourceRoot, 'services/appService.js'), 'utf8');

  assert.match(appService, /setDragRegions:/);
  assert.match(appService, /setCurrentTabDragRegions:/);
  assert.equal(fs.existsSync(path.join(sourceRoot, 'compat/appService.js')), false);
});

test('Vue production code does not import the compatibility scripting service', () => {
  const violations = sourceFiles(sourceRoot)
    .filter((filePath) => !filePath.endsWith(`${path.sep}services${path.sep}scriptingService.js`))
    .filter((filePath) =>
      fs.readFileSync(filePath, 'utf8').includes('services/scriptingService.js')
    );

  assert.deepEqual(violations, []);
});

test('Vue production code does not import the compatibility project-settings service', () => {
  const violations = sourceFiles(sourceRoot)
    .filter(
      (filePath) => !filePath.endsWith(`${path.sep}services${path.sep}projectSettingsService.js`)
    )
    .filter((filePath) =>
      fs.readFileSync(filePath, 'utf8').includes('services/projectSettingsService.js')
    );

  assert.deepEqual(violations, []);
});

test('project lifecycle callers use editorApi for pure project operations', () => {
  const pureMethods = [
    'getProjectLoadStatus',
    'getAppVersion',
    'getRecentProjects',
    'choosePortableSceneTarget',
    'openProjectFile',
    'createWorldProject',
    'createMultiplayerProject',
    'setProjectMode',
  ];
  const violations = sourceFiles(sourceRoot)
    .filter(
      (filePath) => !filePath.endsWith(`${path.sep}services${path.sep}projectLauncherService.js`)
    )
    .map((filePath) => ({ filePath, source: fs.readFileSync(filePath, 'utf8') }))
    .flatMap(({ filePath, source }) =>
      pureMethods
        .filter((method) => source.includes(`projectLauncherService.${method}`))
        .map((method) => `${filePath}: ${method}`)
    );

  assert.deepEqual(violations, []);
});

test('FileManager uses editorApi for file operations', () => {
  const source = fs.readFileSync(path.join(sourceRoot, 'views/sidebar/FileManager.vue'), 'utf8');

  assert.doesNotMatch(source, /fileService\./);
  assert.match(source, /editorApi\.files\./);
});
