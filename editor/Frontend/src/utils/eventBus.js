import { isCurrentWindowEvent } from '../services/appService.js';
/**
 * 事件总线 —— 同一 JS 上下文内的发布-订阅
 *
 * 消息流：
 *   C++ → ExecuteJavaScript
 *     → window.__coronaEmit(event, ...args)        ← 仅主 Tab 收到
 *   pop-out Tab 收到 C++ cross-tab broadcast:
 *     → window.__coronaEmit(event, ...args, {_fromCross:1})
 *     → 内部 emit → pop-out Tab 内的面板组件收到
 */
export const coronaEventBus = {
  _handlers: {},

  on(event, handler) {
    if (!this._handlers[event]) {
      this._handlers[event] = [];
    }
    this._handlers[event].push(handler);
  },

  off(event, handler) {
    if (!this._handlers[event]) return;
    if (!handler) {
      delete this._handlers[event];
    } else {
      this._handlers[event] = this._handlers[event].filter((h) => h !== handler);
    }
  },

  emit(event, ...args) {
    if (!this._handlers[event]) return;
    for (const h of this._handlers[event]) {
      try {
        h(...args);
      } catch (e) {
        console.error(`[coronaEventBus] handler error for "${event}":`, e);
      }
    }
  },
};

/**
 * 统一入口：C++ ExecuteJavaScript 调用
 */
window.__coronaEmit = (event, ...rest) => {
  // 检查最后一个参数是否为选项对象 {_fromCross: 1}
  const last = rest.length > 0 ? rest[rest.length - 1] : undefined;
  const isCross = last && typeof last === 'object' && last._fromCross;
  const args = isCross ? rest.slice(0, -1) : rest;

  if (args[0]?.uiGeneration !== undefined && !isCurrentWindowEvent(args[0])) return;
  coronaEventBus.emit(event, ...args);
};
