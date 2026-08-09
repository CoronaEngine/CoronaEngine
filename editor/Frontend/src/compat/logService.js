/**
 * Compatibility facade for the disabled legacy log lifecycle hooks.
 *
 * Logging is owned by the application/runtime layers; these methods remain
 * no-ops so existing external hosts can keep their historical lifecycle calls.
 */
export const logService = {
  setLogReady: () => Promise.resolve({ success: true, disabled: true }),
  setLogClose: () => Promise.resolve({ success: true, disabled: true }),
};
