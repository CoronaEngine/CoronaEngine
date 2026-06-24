import { createApp } from 'vue';
import { createPinia } from 'pinia';
import App from './App.vue';
import Router from './router/index.js';
import { i18n, setupLocaleSync } from './i18n/index.js';
import { setupDomTranslation } from './i18n/domTranslator.js';
import './style.css';
import 'blockly/blocks';

const app = createApp(App);
app.use(createPinia());
app.use(Router);
app.use(i18n);
setupLocaleSync();
setupDomTranslation();

// 全局自定义指令:点击元素外部时触发回调
// 用于下拉菜单/弹出层在点击外部时自动关闭
// 用法: <div v-click-outside="onClose"> ... </div>
//       <div v-click-outside="() => visible = false"> ... </div>
app.directive('click-outside', {
  mounted(el, binding) {
    const handler = (event) => {
      // el.contains 检查点击目标是否在元素内部
      // 若不在内部,触发 binding.value 回调
      if (!el.contains(event.target)) {
        const fn = binding.value;
        if (typeof fn === 'function') {
          fn(event);
        }
      }
    };
    // 缓存到 el 上,unmounted 时移除
    el.__clickOutsideHandler__ = handler;
    // 延迟一帧再绑定,避免与触发打开的 click 事件重叠导致立即关闭
    setTimeout(() => {
      document.addEventListener('mousedown', handler, true);
      document.addEventListener('touchstart', handler, true);
    }, 0);
  },
  unmounted(el) {
    const handler = el.__clickOutsideHandler__;
    if (handler) {
      document.removeEventListener('mousedown', handler, true);
      document.removeEventListener('touchstart', handler, true);
      el.__clickOutsideHandler__ = null;
    }
  },
});

app.mount('#app');
