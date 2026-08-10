/** Canonical no-op facade for the retired legacy log lifecycle hooks. */
export const logService = {
  setLogReady: () => Promise.resolve({ success: true, disabled: true }),
  setLogClose: () => Promise.resolve({ success: true, disabled: true }),
};
