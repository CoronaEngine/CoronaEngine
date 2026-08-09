import { editorApi } from '../api/editorApi.js';

/** Compatibility facade for historical file-manager imports. */
export const fileService = {
  getProjectInfo: () => editorApi.files.getProjectInfo(),
  getFiles: (relPath) => editorApi.files.getFiles(relPath),
  getFileTree: (relPath) => editorApi.files.getFileTree(relPath),
  createFolder: (path, folderName) => editorApi.files.createFolder(path, folderName),
  createFile: (path, fileName, type) => editorApi.files.createFile(path, fileName, type),
  deleteItem: (path) => editorApi.files.deleteItem(path),
  renameItem: (oldPath, newName) => editorApi.files.renameItem(oldPath, newName),
  openFile: (filePath, fileType) => editorApi.files.openFile(filePath, fileType),
};
