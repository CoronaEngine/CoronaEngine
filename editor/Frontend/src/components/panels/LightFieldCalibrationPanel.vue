<!-- components/panels/LightFieldCalibrationPanel.vue
     光场 3D UI 手动标定 dock 面板。暴露 Vision 光场语义参数 (pe/angle/offset) + UI 专有的
     视差增益 parallaxScale。本面板不持有相机句柄（dock 面板拿不到 props）——改值后通过
     coronaEventBus 发出 'viewport-ui-calibration-changed'，由 MainPage 通过视口快速通道
     使用活动相机句柄下发；C++ 边界统一换算成 warp 的 ViewportUiCalibration。 -->
<template>
  <div class="lfd-calib no-drag">
    <div class="lfd-calib__group">
      <div class="lfd-calib__group-label">Lenticular (Vision)</div>
      <div class="lfd-calib__row">
        <span class="lfd-calib__name">pe</span>
        <NumberInputWithSlider
          v-model="form.pe"
          :step="0.001"
          :min="1"
          :max="64"
          :format="fmt3"
          @change="apply"
        />
      </div>
      <div class="lfd-calib__row">
        <span class="lfd-calib__name">angle</span>
        <NumberInputWithSlider
          v-model="form.angle"
          :step="0.0001"
          :min="-1.5"
          :max="1.5"
          :format="fmt4"
          @change="apply"
        />
      </div>
      <div class="lfd-calib__row">
        <span class="lfd-calib__name">offset</span>
        <NumberInputWithSlider
          v-model="form.offset"
          :step="0.001"
          :min="-64"
          :max="64"
          :format="fmt3"
          @change="apply"
        />
      </div>
    </div>

    <div class="lfd-calib__group">
      <div class="lfd-calib__group-label">Disparity</div>
      <div class="lfd-calib__row">
        <span class="lfd-calib__name">parallax</span>
        <NumberInputWithSlider
          v-model="form.parallaxScale"
          :step="0.1"
          :min="-64"
          :max="64"
          :format="fmt2"
          @change="apply"
        />
      </div>
    </div>

    <button class="lfd-calib__reset no-drag" type="button" @click="resetDefault">
      重置为默认
    </button>
  </div>
</template>

<script setup>
import { onMounted, reactive, toRaw } from 'vue';
import NumberInputWithSlider from '@/components/ui/NumberInputWithSlider.vue';
import { coronaEventBus } from '@/utils/eventBus.js';
import {
  createViewportUiCalibrationStore,
  normalizeLightFieldCalibration,
  DEFAULT_LIGHT_FIELD_CALIBRATION,
} from '@/utils/viewportUiMode.js';

// dock 面板与 MainPage 用同一固定 key 持久化（标定按显示而非按相机），保证两端一致。
const DESCRIPTOR = {};
const store = createViewportUiCalibrationStore();
const form = reactive(normalizeLightFieldCalibration(store.get(DESCRIPTOR)));

const fmt2 = (v) => Number(v).toFixed(2);
const fmt3 = (v) => Number(v).toFixed(3);
const fmt4 = (v) => Number(v).toFixed(4);

const apply = () => {
  const calibration = { ...toRaw(form) };
  store.set(DESCRIPTOR, calibration);
  coronaEventBus.emit('viewport-ui-calibration-changed', calibration);
};

const resetDefault = () => {
  Object.assign(form, normalizeLightFieldCalibration(DEFAULT_LIGHT_FIELD_CALIBRATION));
  apply();
};

// 面板打开（挂载）即把当前标定推送一次，让 warp 立刻拿到值。
onMounted(apply);
</script>

<style scoped>
.lfd-calib {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 10px 12px;
  height: 100%;
  box-sizing: border-box;
  overflow-y: auto;
  background: #181818;
  color: #e0e0e0;
}

.lfd-calib__group {
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.lfd-calib__group-label {
  color: #9a9a9a;
  font-size: 9px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
}

.lfd-calib__row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.lfd-calib__name {
  color: #e0e0e0;
  font-size: 10px;
  min-width: 52px;
}

.lfd-calib__reset {
  align-self: flex-start;
  margin-top: 2px;
  padding: 3px 10px;
  font-size: 10px;
  color: #cbd5e1;
  background: #2a2a2a;
  border: 1px solid #3c3c3c;
  border-radius: 4px;
  cursor: pointer;
}

.lfd-calib__reset:hover {
  background: #353535;
  border-color: #d8b86c;
}
</style>
