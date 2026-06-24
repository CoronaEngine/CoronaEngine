<template>
  <div class="rounded-lg overflow-hidden relative bg-[#282828]/70 flex-1 min-h-0 h-full w-full flex flex-col">
    <DockTitleBar
      v-if="!isDocked"
      title="详情"
      extraClass="bg-[#84A65B] rounded-t-md text-sm"
      routePath="/Object"
      @close="CloseFloat"
    />

    <!-- 主标签页 - 场景、单位、模型 -->
    <div class="bg-[#3c3c3c]/60 border-b border-[#1a1a1a]/30">
      <div class="flex px-2">
        <!-- 场景标签 -->
        <div
          class="flex-1 px-4 py-2 cursor-pointer transition-all duration-200 ease-in-out text-center text-xs"
          :class="{
            'bg-[#264f78]/60 text-white border-b-2 border-[#84a65b]': mainActiveTab === 'scene',
            'hover:bg-[#545454] text-[#e0e0e0]': mainActiveTab !== 'scene',
          }"
          @click="switchMainTab('scene')"
        >
          <span class="select-none font-medium">场景</span>
        </div>

        <!-- 单位标签 -->
        <div
          class="flex-1 px-4 py-2 cursor-pointer transition-all duration-200 ease-in-out text-center text-xs"
          :class="{
            'bg-[#264f78]/60 text-white border-b-2 border-[#84a65b]':
              mainActiveTab === 'actor' && currentActorFile,
            'bg-[#3c3c3c]/30 text-gray-500 border-b-2 border-[#84a65b]/30':
              mainActiveTab === 'actor' && !currentActorFile,
            'hover:bg-[#545454] text-[#e0e0e0]': mainActiveTab !== 'actor',
          }"
          @click="switchMainTab('actor')"
        >
          <span class="select-none font-medium">单位</span>
        </div>

        <!-- 模型标签 -->
        <div
          class="flex-1 px-4 py-2 cursor-pointer transition-all duration-200 ease-in-out text-center text-xs"
          :class="{
            'bg-[#264f78]/60 text-white border-b-2 border-[#84a65b]':
              mainActiveTab === 'model' && currentModelFile,
            'bg-[#3c3c3c]/30 text-gray-500 border-b-2 border-[#84a65b]/30':
              mainActiveTab === 'model' && !currentModelFile,
            'hover:bg-[#545454] text-[#e0e0e0]': mainActiveTab !== 'model',
          }"
          @click="switchMainTab('model')"
        >
          <span class="select-none font-medium">模型</span>
        </div>

        <!-- 时间轴标签 -->
        <div
          class="flex-1 px-4 py-2 cursor-pointer transition-all duration-200 ease-in-out text-center text-xs"
          :class="{
            'bg-[#264f78]/60 text-white border-b-2 border-[#84a65b]': mainActiveTab === 'timeline',
            'hover:bg-[#545454] text-[#e0e0e0]': mainActiveTab !== 'timeline',
          }"
          @click="switchMainTab('timeline')"
        >
          <span class="select-none font-medium">时间轴</span>
        </div>
      </div>
    </div>

    <!-- 主内容区域 -->
    <div class="flex flex-col flex-1 bg-[#282828]/70">
      <!-- 当前文件信息 -->
      <div class="flex items-center gap-2 px-2 py-1.5 bg-[#3c3c3c]/60 border-b border-[#1a1a1a]/30">
        <div class="text-[10px] text-[#909090] truncate flex-1">
          <span class="text-[#84a65b] font-bold">{{ currentFileInfo }}</span>
        </div>
      </div>

      <!-- 未打开文件提示 - 只对单位和模型标签显示 -->
      <div
        v-if="
          (mainActiveTab === 'actor' && !currentActorFile) ||
          (mainActiveTab === 'model' && !currentModelFile)
        "
        class="flex-1 flex items-center justify-center text-[#909090] text-xs"
      >
        <span>未打开文件</span>
      </div>

      <!-- 内容区域 - 仅在文件已打开时显示 -->
      <template v-else>
        <!-- 次级标签页（根据主标签动态显示） -->
        <div
          v-if="mainActiveTab === 'scene'"
          class="flex bg-[#3c3c3c]/30 border-b border-[#1a1a1a]/30"
        >
          <button
            v-for="tab in sceneTabs"
            :key="tab.id"
            :class="[
              ActiveSubTab === tab.id
                ? 'bg-[#264f78]/60 text-white'
                : 'text-[#909090] hover:bg-[#545454] hover:text-[#e0e0e0]',
            ]"
            class="flex-1 px-2 py-1 text-xs transition-colors duration-200"
            @click="ActiveSubTab = tab.id"
          >
            {{ tab.label }}
          </button>
        </div>

        <div
          v-else-if="mainActiveTab === 'actor'"
          class="flex bg-[#3c3c3c]/30 border-b border-[#1a1a1a]/30"
        >
          <button
            v-for="tab in actorTabs"
            :key="tab.id"
            :class="[
              ActiveSubTab === tab.id
                ? 'bg-[#264f78]/60 text-white'
                : 'text-[#909090] hover:bg-[#545454] hover:text-[#e0e0e0]',
            ]"
            class="flex-1 px-2 py-1 text-xs transition-colors duration-200"
            @click="ActiveSubTab = tab.id"
          >
            {{ tab.label }}
          </button>
        </div>

        <div
          v-else-if="mainActiveTab === 'model'"
          class="flex bg-[#3c3c3c]/30 border-b border-[#1a1a1a]/30"
        >
          <button
            v-for="tab in modelTabs"
            :key="tab.id"
            :class="[
              ActiveSubTab === tab.id
                ? 'bg-[#264f78]/60 text-white'
                : 'text-[#909090] hover:bg-[#545454] hover:text-[#e0e0e0]',
            ]"
            class="flex-1 px-2 py-1 text-xs transition-colors duration-200"
            @click="ActiveSubTab = tab.id"
          >
            {{ tab.label }}
          </button>
        </div>

        <!-- ========== 内容区域 ========== -->
        <div class="flex-1 overflow-auto bg-[#282828]/50 p-2">
          <!-- ===== 场景内容 ===== -->
          <template v-if="mainActiveTab === 'scene'">
            <!-- 场景 - 基础信息 -->
            <div v-show="ActiveSubTab === 'Basic'" class="space-y-2 text-xs">
              <!-- 光照设置 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-yellow-600">
                <div class="flex items-center justify-between mb-2">
                  <label class="text-[#e0e0e0] font-medium text-xs">光照</label>
                  <div class="flex items-center space-x-2">
                    <label class="text-[#909090] text-[10px]">启用</label>
                    <input
                      v-model="sceneData.light.enabled"
                      type="checkbox"
                      class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b] w-3 h-3"
                      @change="updateLightDirection"
                    />
                  </div>
                </div>

                <div v-if="sceneData.light.enabled" class="space-y-1">
                  <!-- 方向标签 -->
                  <div class="text-[#909090] text-[10px] mb-1">方向</div>
                  <!-- 方向输入框 -->
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <NumberInputWithSlider
                        v-model="sceneData.light.direction.x"
                        :step="0.1"
                        :min="-10"
                        :max="10"
                        @change="updateLightDirection"
                      />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <NumberInputWithSlider
                        v-model="sceneData.light.direction.y"
                        :step="0.1"
                        :min="-10"
                        :max="10"
                        @change="updateLightDirection"
                      />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <NumberInputWithSlider
                        v-model="sceneData.light.direction.z"
                        :step="0.1"
                        :min="-10"
                        :max="10"
                        @change="updateLightDirection"
                      />
                    </div>
                  </div>
                </div>
              </div>

              <!-- 网格设置 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-cyan-600">
                <div class="flex items-center justify-between">
                  <label class="text-[#e0e0e0] font-medium text-xs">网格</label>
                  <div class="flex items-center space-x-2">
                    <label class="text-[#909090] text-[10px]">显示</label>
                    <input
                      v-model="sceneData.grid.enabled"
                      type="checkbox"
                      class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b] w-3 h-3"
                      @change="updateFloorGrid"
                    />
                  </div>
                </div>
              </div>
            </div>

            <!-- 场景 - 地形 -->
            <div v-show="ActiveSubTab === 'Terrain'" class="space-y-2 text-xs">
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-green-600">
                <div class="flex items-center mb-2">
                  <label class="text-[#e0e0e0] font-medium w-16">地形</label>
                  <select
                    v-model="sceneData.terrain.type"
                    class="flex-1 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] focus:border-[#84a65b] focus:outline-none text-[10px]"
                  >
                    <option value="none">无</option>
                    <option value="plane">平面</option>
                    <option value="custom">自定义</option>
                  </select>
                </div>

                <div v-if="sceneData.terrain.type === 'custom'" class="mt-2 flex space-x-1">
                  <input
                    v-model="sceneData.terrain.path"
                    type="text"
                    readonly
                    class="flex-1 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px]"
                    placeholder="选择地形文件"
                  />
                  <button
                    class="px-2 py-1 bg-[#545454] text-[#e0e0e0] rounded hover:bg-[#686868] whitespace-nowrap text-[10px]"
                    @click="selectTerrainFile"
                  >
                    浏览
                  </button>
                </div>

                <div v-if="sceneData.terrain.type === 'plane'" class="mt-2">
                  <div class="flex items-center space-x-2">
                    <label class="text-[#909090] w-16">尺寸</label>
                    <NumberInputWithSlider
                      v-model="sceneData.terrain.size"
                      :step="0.1"
                      :min="0.1"
                      :max="100"
                    />
                  </div>
                </div>
              </div>
            </div>

            <!-- 场景 - 脚本 -->
            <div v-show="ActiveSubTab === 'Script'" class="space-y-2 text-xs">
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-blue-600">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">脚本</label>
                  <div class="flex-1 flex space-x-1">
                    <input
                      v-model="sceneData.script.path"
                      type="text"
                      readonly
                      class="flex-1 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px]"
                      placeholder="选择脚本文件"
                    />
                    <button
                      class="px-2 py-1 bg-[#545454] text-[#e0e0e0] rounded hover:bg-[#686868] whitespace-nowrap text-[10px]"
                      @click="selectSceneScript"
                    >
                      浏览
                    </button>
                  </div>
                </div>
              </div>
            </div>
          </template>

          <!-- ===== 单位内容 ===== -->
          <template v-else-if="mainActiveTab === 'actor'">
            <!-- 单位 - 基础信息 -->
            <div v-show="ActiveSubTab === 'Basic'" class="space-y-2 text-xs">
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-[#84a65b]">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">别名</label>
                  <input
                    v-model="actorAliasDraft"
                    type="text"
                    class="flex-1 min-w-0 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px] focus:border-[#84a65b] focus:outline-none"
                    placeholder="Actor 别名"
                    :disabled="actorAliasSaving"
                    @keydown.enter.prevent="commitActorAlias"
                    @keydown.esc.prevent="resetActorAliasDraft"
                  />
                  <button
                    type="button"
                    class="px-2 py-1 rounded whitespace-nowrap text-[10px] transition-colors"
                    :class="actorAliasDirty && !actorAliasSaving ? 'bg-[#84a65b] text-[#101510] hover:bg-[#95b96a]' : 'bg-[#545454] text-[#909090] cursor-not-allowed'"
                    :disabled="!actorAliasDirty || actorAliasSaving"
                    @click="commitActorAlias"
                  >
                    {{ actorAliasSaving ? '保存中' : '保存' }}
                  </button>
                </div>
                <div v-if="actorAliasError" class="mt-1 text-[10px] text-red-300">
                  {{ actorAliasError }}
                </div>
              </div>

              <div class="bg-[#3c3c3c]/50 p-2 rounded">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">场景</label>
                  <div class="flex-1 text-[#909090] text-[10px] bg-[#1a1a1a] p-1 rounded">
                    {{ actorData.parentScene || '未指定' }}
                  </div>
                </div>
              </div>

              <div class="bg-[#3c3c3c]/50 p-2 rounded">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">文件</label>
                  <div class="flex-1 text-[#909090] truncate text-[10px]" :title="currentActorFile">
                    {{ currentActorFile.split('/').pop() }}
                  </div>
                </div>
              </div>

              <div
                class="bg-[#3c3c3c]/50 p-2 rounded border-l-2"
                :class="actorData.follow_camera ? 'border-cyan-500' : 'border-[#545454]'"
              >
                <div class="flex items-center justify-between gap-2">
                  <label class="text-[#e0e0e0] font-medium w-16">渲染空间</label>
                  <div class="flex bg-[#1a1a1a] rounded p-0.5">
                    <button
                      type="button"
                      class="px-2 py-1 rounded text-[10px] transition-colors"
                      :class="!actorData.follow_camera ? 'bg-[#545454] text-[#ffffff]' : 'text-[#909090] hover:text-[#e0e0e0]'"
                      @click="setActorRenderSpace(false)"
                    >
                      场景
                    </button>
                    <button
                      type="button"
                      class="px-2 py-1 rounded text-[10px] transition-colors"
                      :class="actorData.follow_camera ? 'bg-cyan-600 text-[#ffffff]' : 'text-[#909090] hover:text-[#e0e0e0]'"
                      @click="setActorRenderSpace(true)"
                    >
                      屏幕 UI
                    </button>
                  </div>
                </div>
              </div>
            </div>

            <!-- 单位 - 模型 -->
            <div v-show="ActiveSubTab === 'Model'" class="space-y-2 text-xs">
              <!-- 模型路径 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-green-600">
                <div class="flex items-center space-x-2 mb-2">
                  <label class="text-[#e0e0e0] font-medium w-16">模型</label>
                  <div class="flex-1 flex space-x-1">
                    <input
                      v-model="actorData.model.path"
                      type="text"
                      readonly
                      class="flex-1 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px]"
                      placeholder="选择模型文件"
                    />
                    <button
                      class="px-2 py-1 bg-[#545454] text-[#e0e0e0] rounded hover:bg-[#686868] whitespace-nowrap text-[10px]"
                      @click="selectModelFile"
                    >
                      浏览
                    </button>
                  </div>
                </div>

                <div
                  class="h-16 bg-[#1a1a1a] rounded flex items-center justify-center text-[#909090] text-[10px]"
                >
                  <span v-if="!actorData.model.path">未选择模型</span>
                  <span v-else class="truncate px-2">{{ actorData.model.path }}</span>
                </div>
              </div>

              <!-- 变换 - 只有选择了模型才显示 -->
              <template v-if="actorData.model.path">
                <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-blue-600">
                  <!-- 相机锁定开关 -->
                  <div class="flex items-center justify-between mb-2 pb-2 border-b border-[#1a1a1a]/50">
                    <div class="flex items-center gap-1">
                      <span class="text-pink-400 text-[10px]">&#9679;</span>
                      <label class="text-[#e0e0e0] font-medium text-[10px]">相机锁定</label>
                    </div>
                    <div class="flex items-center space-x-2">
                      <label class="text-[#909090] text-[10px]">启用</label>
                      <input
                        v-model="actorData.camera_lock.lock_to_camera"
                        type="checkbox"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-pink-500 w-3 h-3"
                        @change="updateCameraLock"
                      />
                    </div>
                  </div>

                  <!-- 相机锁定偏移（启用后显示） -->
                  <div v-if="actorData.camera_lock.lock_to_camera" class="mb-2 pb-2 border-b border-[#1a1a1a]/50">
                    <div class="text-pink-400 text-[10px] mb-1">锁定偏移</div>
                    <div class="grid grid-cols-3 gap-1 mb-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <NumberInputWithSlider
                          v-model="actorData.camera_lock.position_offset.x"
                          :step="0.1" :min="-20" :max="20"
                          @change="updateCameraLockOffset"
                        />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <NumberInputWithSlider
                          v-model="actorData.camera_lock.position_offset.y"
                          :step="0.1" :min="-20" :max="20"
                          @change="updateCameraLockOffset"
                        />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <NumberInputWithSlider
                          v-model="actorData.camera_lock.position_offset.z"
                          :step="0.1" :min="-20" :max="20"
                          @change="updateCameraLockOffset"
                        />
                      </div>
                    </div>
                  </div>

                  <label class="text-[#e0e0e0] font-medium block mb-1 text-[10px]">变换 [v3]</label>

                  <!-- 位置 -->
                  <div class="mb-1">
                    <div class="text-[#909090] text-[10px] mb-0.5">位置</div>
                    <div class="grid grid-cols-3 gap-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.position.x"
                          :step="0.1"
                          :min="-100"
                          :max="100"
                          @update:model-value="(value) => updateActorTransformFast('SetPosition', 'x', value)"
                          @change="() => updateActorTransform('SetPosition')"
                        />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.position.y"
                          :step="0.1"
                          :min="-100"
                          :max="100"
                          @update:model-value="(value) => updateActorTransformFast('SetPosition', 'y', value)"
                          @change="() => updateActorTransform('SetPosition')"
                        />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.position.z"
                          :step="0.1"
                          :min="-100"
                          :max="100"
                          @update:model-value="(value) => updateActorTransformFast('SetPosition', 'z', value)"
                          @change="() => updateActorTransform('SetPosition')"
                        />
                      </div>
                    </div>
                  </div>

                  <!-- 旋转 -->
                  <div class="mb-1">
                    <div class="text-[#909090] text-[10px] mb-0.5">旋转</div>
                    <div class="grid grid-cols-3 gap-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.rotation.x"
                          :step="0.1"
                          :min="-360"
                          :max="360"
                          @update:model-value="(value) => updateActorTransformFast('SetRotation', 'x', value)"
                          @change="() => updateActorTransform('SetRotation')"
                        />
                      </div>

                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.rotation.y"
                          :step="0.1"
                          :min="-360"
                          :max="360"
                          @update:model-value="(value) => updateActorTransformFast('SetRotation', 'y', value)"
                          @change="() => updateActorTransform('SetRotation')"
                        />
                      </div>

                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.rotation.z"
                          :step="0.1"
                          :min="-360"
                          :max="360"
                          @update:model-value="(value) => updateActorTransformFast('SetRotation', 'z', value)"
                          @change="() => updateActorTransform('SetRotation')"
                        />
                      </div>
                    </div>
                  </div>

                  <!-- 缩放 -->
                  <div>
                    <div class="text-[#909090] text-[10px] mb-0.5">缩放</div>
                    <div class="grid grid-cols-3 gap-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.scale.x"
                          :step="0.1"
                          :min="0.01"
                          :max="10"
                          @update:model-value="(value) => updateActorTransformFast('SetScale', 'x', value)"
                          @change="() => updateActorTransform('SetScale')"
                        />
                      </div>

                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.scale.y"
                          :step="0.1"
                          :min="0.01"
                          :max="10"
                          @update:model-value="(value) => updateActorTransformFast('SetScale', 'y', value)"
                          @change="() => updateActorTransform('SetScale')"
                        />
                      </div>

                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <NumberInputWithSlider
                          v-model="actorData.transform.scale.z"
                          :step="0.1"
                          :min="0.01"
                          :max="10"
                          @update:model-value="(value) => updateActorTransformFast('SetScale', 'z', value)"
                          @change="() => updateActorTransform('SetScale')"
                        />
                      </div>
                    </div>
                  </div>
                </div>

                <!-- 碰撞选项 -->
                <div
                  v-if="actorData.hasGeometry"
                  class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-orange-600"
                >
                  <div class="flex items-center justify-between">
                    <label class="text-[#e0e0e0] font-medium">碰撞</label>
                    <div class="flex items-center space-x-4">
                      <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                        <input
                          v-model="actorData.collision.type"
                          type="radio"
                          value="none"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                          @change="updateActorCollision"
                          @update:model-value="updateActorCollisionFast"
                        />
                        <span>无</span>
                      </label>
                      <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                        <input
                          v-model="actorData.collision.type"
                          type="radio"
                          value="box"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                          @change="updateActorCollision"
                          @update:model-value="updateActorCollisionFast"
                        />
                        <span>包围盒</span>
                      </label>
                      <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                        <input
                          v-model="actorData.collision.type"
                          type="radio"
                          value="mesh"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                          @change="updateActorCollision"
                          @update:model-value="updateActorCollisionFast"
                        />
                        <span>网格</span>
                      </label>
                    </div>
                  </div>
                </div>

                <!-- 物理属性 -->
                <div
                  v-if="actorData.hasMechanics"
                  class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-purple-600"
                >
                  <label class="text-[#e0e0e0] font-medium block mb-1 text-[10px]">物理</label>
                  <!-- 启用物理开关 -->
                  <div class="mb-1 flex items-center justify-between">
                    <span class="text-[#909090] text-[10px]">启用物理</span>
                    <label class="relative inline-flex items-center cursor-pointer">
                      <input
                        type="checkbox"
                        v-model="actorData.mechanics.physics_enabled"
                        class="sr-only"
                        @change="() => updateActorMechanics('SetPhysicsEnabled')"
                        @update:model-value="() => updateActorMechanicsFast(PROPERTY.PhysicsEnabled)"
                      />
                      <div
                        class="relative w-7 h-4 rounded-full transition-colors"
                        :class="actorData.mechanics.physics_enabled ? 'bg-[#84a65b]/60' : 'bg-[#1a1a1a]'"
                      >
                        <span
                          class="absolute top-[2px] left-[2px] h-3 w-3 rounded-full transition-all"
                          :class="actorData.mechanics.physics_enabled ? 'translate-x-3 bg-[#84a65b]' : 'translate-x-0 bg-[#555]'"
                        ></span>
                      </div>
                    </label>
                  </div>
                  <!-- 质量 -->
                  <div class="mb-1">
                    <div class="text-[#909090] text-[10px] mb-0.5">质量</div>
                    <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                      <NumberInputWithSlider
                        v-model="actorData.mechanics.mass"
                        :step="0.1"
                        :min="0.01"
                        :max="100"
                        @change="() => updateActorMechanics('SetMass')"
                        @update:model-value="() => updateActorMechanicsFast(PROPERTY.Mass)"
                      />
                    </div>
                  </div>
                  <!-- 弹性系数 -->
                  <div class="mb-1">
                    <div class="text-[#909090] text-[10px] mb-0.5">弹性系数</div>
                    <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                      <NumberInputWithSlider
                        v-model="actorData.mechanics.restitution"
                        :step="0.05"
                        :min="0"
                        :max="1"
                        @change="() => updateActorMechanics('SetRestitution')"
                        @update:model-value="() => updateActorMechanicsFast(PROPERTY.Restitution)"
                      />
                    </div>
                  </div>
                  <!-- 阻尼 -->
                  <div>
                    <div class="text-[#909090] text-[10px] mb-0.5">阻尼</div>
                    <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                      <NumberInputWithSlider
                        v-model="actorData.mechanics.damping"
                        :step="0.01"
                        :min="0"
                        :max="1"
                        @change="() => updateActorMechanics('SetDamping')"
                        @update:model-value="() => updateActorMechanicsFast(PROPERTY.Damping)"
                      />
                    </div>
                  </div>
                  <!-- 轴锁定 - 平移 -->
                  <div class="mt-2 pt-1 border-t border-[#1a1a1a]/50">
                    <div class="text-[#909090] text-[10px] mb-1">平移锁定</div>
                    <div class="grid grid-cols-3 gap-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.linear_lock[0]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-red-500 w-3 h-3"
                          @change="() => { updateLinearLockFast(); updateActorMechanics('SetLinearLock'); }" />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.linear_lock[1]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-blue-500 w-3 h-3"
                          @change="() => { updateLinearLockFast(); updateActorMechanics('SetLinearLock'); }" />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.linear_lock[2]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-green-500 w-3 h-3"
                          @change="() => { updateLinearLockFast(); updateActorMechanics('SetLinearLock'); }" />
                      </div>
                    </div>
                  </div>
                  <!-- 轴锁定 - 旋转 -->
                  <div class="mt-1">
                    <div class="text-[#909090] text-[10px] mb-1">旋转锁定</div>
                    <div class="grid grid-cols-3 gap-1">
                      <div class="flex items-center gap-1">
                        <label class="text-red-400 text-[10px] w-3">X</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.angular_lock[0]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-red-500 w-3 h-3"
                          @change="() => { updateAngularLockFast(); updateActorMechanics('SetAngularLock'); }" />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-blue-400 text-[10px] w-3">Y</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.angular_lock[1]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-blue-500 w-3 h-3"
                          @change="() => { updateAngularLockFast(); updateActorMechanics('SetAngularLock'); }" />
                      </div>
                      <div class="flex items-center gap-1">
                        <label class="text-green-400 text-[10px] w-3">Z</label>
                        <input type="checkbox"
                          v-model="actorData.mechanics.angular_lock[2]"
                          class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-green-500 w-3 h-3"
                          @change="() => { updateAngularLockFast(); updateActorMechanics('SetAngularLock'); }" />
                      </div>
                    </div>
                  </div>
                </div>
              </template>

              <!-- 未选择模型时的提示 -->
              <div
                v-else
                class="bg-[#3c3c3c]/50 p-4 rounded text-center text-[#909090] text-[10px]"
              >
                请先选择模型文件
              </div>
            </div>

            <!-- 单位 - 积木 -->
            <div v-show="ActiveSubTab === 'Blockly'" class="flex flex-col" style="height: 400px;">
              <BlocklyWorkspace
                v-if="actorData.name"
                ref="actorBlocklyRef"
                :actorName="actorData.name"
                :sceneName="actorData.parentScene || sceneData.name"
                embedded
              />
              <div v-else class="flex items-center justify-center h-full text-[#909090] text-xs">
                请先选中一个物体
              </div>
            </div>

            <!-- 单位 - 脚本 -->
            <div v-show="ActiveSubTab === 'Script'" class="space-y-2 text-xs">
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-blue-600">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">脚本</label>
                  <div class="flex-1 flex space-x-1">
                    <input
                      v-model="actorData.script.path"
                      type="text"
                      readonly
                      class="flex-1 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px]"
                      placeholder="选择脚本文件"
                    />
                    <button
                      class="px-2 py-1 bg-[#545454] text-[#e0e0e0] rounded hover:bg-[#686868] whitespace-nowrap text-[10px]"
                      @click="selectActorScript"
                    >
                      浏览
                    </button>
                  </div>
                </div>
              </div>
            </div>
          </template>

          <!-- ===== 时间轴内容 ===== -->
          <template v-else-if="mainActiveTab === 'timeline'">
            <div class="flex flex-col h-full bg-[#1e1e1e]">
              <!-- 播放控制栏 -->
              <div class="flex items-center gap-1 px-2 py-1.5 bg-[#383838] border-b border-[#555] shrink-0">
                <div class="flex items-center gap-0.5">
                  <button class="w-6 h-6 flex items-center justify-center rounded hover:bg-[#4a4a4a] text-[#ccc] hover:text-white transition-colors" title="跳到开始" @click="seekToStart">
                    <svg class="w-3.5 h-3.5" fill="currentColor" viewBox="0 0 24 24"><path d="M6 6h2v12H6zm3.5 6l8.5 6V6z"/></svg>
                  </button>
                  <button
                    class="w-7 h-7 flex items-center justify-center rounded-full transition-colors mx-0.5"
                    :class="timelineState.playing ? 'bg-red-500 hover:bg-red-400' : 'bg-[#84a65b] hover:bg-[#9bc46d]'"
                    :title="timelineState.playing ? '停止' : '播放'"
                    @click="togglePlayback"
                  >
                    <svg v-if="!timelineState.playing" class="w-3.5 h-3.5 ml-0.5" fill="currentColor" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
                    <svg v-else class="w-3.5 h-3.5" fill="currentColor" viewBox="0 0 24 24"><rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/></svg>
                  </button>
                  <button class="w-6 h-6 flex items-center justify-center rounded hover:bg-[#4a4a4a] text-[#ccc] hover:text-white transition-colors" title="跳到结尾" @click="seekToEnd">
                    <svg class="w-3.5 h-3.5" fill="currentColor" viewBox="0 0 24 24"><path d="M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z"/></svg>
                  </button>
                </div>
                <div class="w-px h-5 bg-[#555] mx-1"></div>
                <div class="flex items-center gap-1 text-xs">
                  <span class="text-[#909090] text-[10px]">时间</span>
                  <input type="text" :value="formatTimelineTime(timelineState.currentTime)" readonly
                    class="w-20 bg-[#2d2d2d] text-[#e0e0e0] text-xs text-center rounded px-1 py-0.5 border border-[#555] font-mono" />
                </div>
                <div class="flex items-center gap-1 text-xs ml-2">
                  <span class="text-[#909090] text-[10px]">帧</span>
                  <input type="text" :value="timelineState.currentFrame" readonly
                    class="w-14 bg-[#2d2d2d] text-[#e0e0e0] text-xs text-center rounded px-1 py-0.5 border border-[#555] font-mono" />
                </div>
                <div class="flex-1"></div>
                <button
                  class="px-2 py-1 text-[10px] text-red-400 hover:bg-red-500/20 hover:text-red-300 rounded transition-colors flex items-center gap-1"
                  title="一键清除所有内容"
                  @click="clearAllTimeline"
                >
                  <svg class="w-3 h-3" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/></svg>
                  清除
                </button>
                <div class="w-px h-5 bg-[#555] mx-1"></div>
                <div class="flex items-center gap-1 text-xs">
                  <span class="text-[#909090] text-[10px]">FPS</span>
                  <select v-model="timelineState.fps"
                    class="bg-[#2d2d2d] text-[#e0e0e0] text-xs rounded px-1 py-0.5 border border-[#555]">
                    <option :value="24">24</option>
                    <option :value="30">30</option>
                    <option :value="60">60</option>
                    <option :value="120">120</option>
                  </select>
                </div>
                <div class="flex items-center gap-1 text-xs ml-2">
                  <span class="text-[#909090] text-[10px]">总帧</span>
                  <input type="text" :value="timelineState.totalFrames" readonly
                    class="w-14 bg-[#2d2d2d] text-[#e0e0e0] text-xs text-center rounded px-1 py-0.5 border border-[#555] font-mono" />
                </div>
              </div>

              <!-- 时间标尺行 -->
              <div class="flex shrink-0 border-b border-[#444]">
                <div class="w-32 shrink-0 bg-[#2d2d2d] border-r border-[#444] px-2 py-1">
                  <div class="h-5"></div>
                </div>
                <div class="flex-1 relative bg-[#2a2a2a] h-5 overflow-hidden" ref="timelineRulerRef"
                  @click="onRulerClick">
                  <div class="absolute inset-0 flex items-end pointer-events-none">
                    <div v-for="tick in timelineTicks" :key="tick.time"
                      class="absolute bottom-0 flex flex-col items-center"
                      :style="{ left: tick.left + '%' }">
                      <div class="h-2 w-px" :class="tick.major ? 'bg-[#888]' : 'bg-[#555]'"></div>
                      <span v-if="tick.major" class="text-[9px] text-[#999] mt-0.5 font-mono select-none">{{ tick.label }}</span>
                    </div>
                  </div>
                  <div class="absolute top-0 bottom-0 w-0.5 bg-red-500 z-10 pointer-events-none"
                    :style="{ left: playheadPosition + '%' }">
                    <div class="w-0 h-0 border-l-[5px] border-r-[5px] border-t-[6px] border-l-transparent border-r-transparent border-t-red-500 -ml-[4.5px]"></div>
                  </div>
                </div>
              </div>

              <!-- 轨道区域 -->
              <div class="flex-1 flex overflow-auto">
                <div class="w-32 shrink-0 bg-[#2d2d2d] border-r border-[#444]">
                  <div v-for="track in timelineTracks" :key="track.id"
                    class="h-10 flex items-center px-2 border-b border-[#3a3a3a] cursor-pointer hover:bg-[#3a3a3a] transition-colors select-none"
                    :class="{ 'bg-[#3a3a3a]': timelineState.selectedTrack === track.id }"
                    @click="timelineState.selectedTrack = track.id">
                    <span class="text-xs mr-1.5">{{ track.icon }}</span>
                    <span class="text-[10px] text-[#888] mr-1">{{ track.expanded ? '▼' : '▶' }}</span>
                    <span class="text-[11px] text-[#ccc] truncate">{{ track.name }}</span>
                  </div>
                </div>
                <div class="flex-1 relative bg-[#1e1e1e]" ref="trackContentRef">
                  <div v-for="tick in timelineTicks" :key="'g'+tick.time"
                    class="absolute top-0 bottom-0 pointer-events-none"
                    :class="tick.major ? 'border-l border-[#333]' : 'border-l border-[#2a2a2a]'"
                    :style="{ left: tick.left + '%' }"></div>
                  <div v-for="track in timelineTracks" :key="track.id"
                    class="h-10 relative border-b border-[#2a2a2a] cursor-crosshair"
                    :class="{ 'bg-[#252525]': timelineState.selectedTrack === track.id }"
                    @mousedown="onTrackMouseDown(track, $event)"
                    @mousemove="onTrackMouseMove(track, $event)"
                    @mouseup="onTrackMouseUp(track, $event)"
                    @mouseleave="onTrackMouseLeave(track, $event)"
                    @click="onTrackClick(track, $event)">
                    <!-- 已添加的片段 -->
                    <div v-for="(clip, ci) in track.clips" :key="ci"
                      class="absolute top-1 bottom-1 rounded-sm cursor-pointer group"
                      :class="clipColorClass(track.type)"
                      :style="{ left: clipTimeToPercent(clip.startFrame) + '%', width: Math.max(clipTimeToPercent(clip.durationFrames) - clipTimeToPercent(0), 1.5) + '%' }"
                      :title="clip.name + ' (' + clip.startFrame + '-' + (clip.startFrame+clip.durationFrames) + ')'"
                      @mousedown.stop
                      @dblclick.stop="deleteClip(track, ci)">
                      <span class="text-[9px] text-white/90 truncate px-1 leading-6 select-none block pointer-events-none">{{ clip.name }}</span>
                    </div>
                    <!-- 已添加的关键帧 -->
                    <div v-for="(kf, ki) in track.keyframes" :key="'kf'+ki"
                      class="absolute top-1/2 -translate-y-1/2 w-2.5 h-2.5 rotate-45 cursor-pointer z-10 hover:scale-125 transition-transform"
                      :class="keyframeColorClass(track.type)"
                      :style="{ left: 'calc(' + clipTimeToPercent(kf.frame) + '% - 5px)' }"
                      :title="'帧 ' + kf.frame + ': ' + (kf.value || '')"
                      @mousedown.stop
                      @dblclick.stop="deleteKeyframe(track, ki)"></div>
                    <!-- 长按拖拽中的预览条 -->
                    <div v-if="clipDragState.active && clipDragState.trackId === track.id"
                      class="absolute top-1 bottom-1 rounded-sm z-20 pointer-events-none"
                      :class="clipPreviewColor(track.type)"
                      :style="clipDragPreviewStyle"></div>
                  </div>
                  <!-- 播放头 -->
                  <div class="absolute top-0 bottom-0 w-0.5 bg-red-500 z-20 pointer-events-none"
                    :style="{ left: playheadPosition + '%' }"></div>
                </div>
              </div>

              <!-- 底部状态栏 -->
              <div class="flex items-center gap-3 px-2 py-1 bg-[#383838] border-t border-[#555] text-[10px] text-[#888] shrink-0">
                <span>时长: {{ formatTimelineTime(timelineState.duration) }}</span>
                <span class="w-px h-3 bg-[#555]"></span>
                <span>帧范围: 0 - {{ timelineState.totalFrames }}</span>
                <span class="w-px h-3 bg-[#555]"></span>
                <span>提示: <span class="text-[#999]">长按拖拽添加片段 · 点击添加关键帧 · 双击删除</span></span>
                <span class="flex-1"></span>
                <span class="text-[#84a65b]">{{ timelineState.playing ? '▶ 播放中' : '' }}</span>
              </div>
            </div>
          </template>

          <!-- ===== 模型内容 ===== -->
          <template v-else-if="mainActiveTab === 'model'">
            <!-- 模型 - 基础信息 -->
            <div v-show="ActiveSubTab === 'Basic'" class="space-y-2 text-xs">
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-[#84a65b]">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">别名</label>
                  <input
                    v-model="modelAliasDraft"
                    type="text"
                    class="flex-1 min-w-0 p-1 bg-[#1a1a1a] text-[#e0e0e0] rounded border border-[#3c3c3c] text-[10px] focus:border-[#84a65b] focus:outline-none"
                    placeholder="模型别名"
                    :disabled="modelAliasSaving"
                    @keydown.enter.prevent="commitModelAlias"
                    @keydown.esc.prevent="resetModelAliasDraft"
                  />
                  <button
                    type="button"
                    class="px-2 py-1 rounded whitespace-nowrap text-[10px] transition-colors"
                    :class="modelAliasDirty && !modelAliasSaving ? 'bg-[#84a65b] text-[#101510] hover:bg-[#95b96a]' : 'bg-[#545454] text-[#909090] cursor-not-allowed'"
                    :disabled="!modelAliasDirty || modelAliasSaving"
                    @click="commitModelAlias"
                  >
                    {{ modelAliasSaving ? '保存中' : '保存' }}
                  </button>
                </div>
                <div v-if="modelAliasError" class="mt-1 text-[10px] text-red-300">
                  {{ modelAliasError }}
                </div>
              </div>

              <div class="bg-[#3c3c3c]/50 p-2 rounded">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">场景</label>
                  <div class="flex-1 text-[#909090] text-[10px] bg-[#1a1a1a] p-1 rounded">
                    {{ modelData.targetScene || '未指定' }}
                  </div>
                </div>
              </div>

              <div class="bg-[#3c3c3c]/50 p-2 rounded">
                <div class="flex items-center space-x-2">
                  <label class="text-[#e0e0e0] font-medium w-16">文件</label>
                  <div class="flex-1 text-[#909090] truncate text-[10px]" :title="currentModelFile">
                    {{ currentModelFile.split('/').pop() }}
                  </div>
                </div>
              </div>

              <div
                class="bg-[#3c3c3c]/50 p-2 rounded border-l-2"
                :class="modelData.follow_camera ? 'border-cyan-500' : 'border-[#545454]'"
              >
                <div class="flex items-center justify-between gap-2">
                  <label class="text-[#e0e0e0] font-medium w-16">渲染空间</label>
                  <div class="flex bg-[#1a1a1a] rounded p-0.5">
                    <button
                      type="button"
                      class="px-2 py-1 rounded text-[10px] transition-colors"
                      :class="!modelData.follow_camera ? 'bg-[#545454] text-[#ffffff]' : 'text-[#909090] hover:text-[#e0e0e0]'"
                      @click="setModelRenderSpace(false)"
                    >
                      场景
                    </button>
                    <button
                      type="button"
                      class="px-2 py-1 rounded text-[10px] transition-colors"
                      :class="modelData.follow_camera ? 'bg-cyan-600 text-[#ffffff]' : 'text-[#909090] hover:text-[#e0e0e0]'"
                      @click="setModelRenderSpace(true)"
                    >
                      屏幕 UI
                    </button>
                  </div>
                </div>
              </div>
            </div>

            <!-- 模型 - 模型设置 -->
            <div v-show="ActiveSubTab === 'Model'" class="space-y-2 text-xs">
              <!-- 模型路径 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-green-600">
                <label class="text-[#e0e0e0] font-medium block mb-1">模型文件</label>
                <div
                  class="text-[#909090] text-[10px] bg-[#1a1a1a] p-1 rounded truncate"
                  :title="currentModelFile"
                >
                  {{ modelData.file }}
                </div>
              </div>

              <!-- 变换 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-blue-600">
                <label class="text-[#e0e0e0] font-medium block mb-1 text-[10px]">默认变换</label>

                <!-- 位置 -->
                <div class="mb-1">
                  <div class="text-[#909090] text-[10px] mb-0.5">位置</div>
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.position.x"
                        :step="0.1"
                        :min="-100"
                        :max="100"
                        @update:model-value="(value) => updateModelTransformFast('SetPosition', 'x', value)"
                        @change="() => updateModelTransform('SetPosition')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.position.y"
                        :step="0.1"
                        :min="-100"
                        :max="100"
                        @update:model-value="(value) => updateModelTransformFast('SetPosition', 'y', value)"
                        @change="() => updateModelTransform('SetPosition')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.position.z"
                        :step="0.1"
                        :min="-100"
                        :max="100"
                        @update:model-value="(value) => updateModelTransformFast('SetPosition', 'z', value)"
                        @change="() => updateModelTransform('SetPosition')"
                      />
                    </div>
                  </div>
                </div>

                <!-- 旋转 -->
                <div class="mb-1">
                  <div class="text-[#909090] text-[10px] mb-0.5">旋转</div>
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.rotation.x"
                        :step="0.1"
                        :min="-360"
                        :max="360"
                        @update:model-value="(value) => updateModelTransformFast('SetRotation', 'x', value)"
                        @change="() => updateModelTransform('SetRotation')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.rotation.y"
                        :step="0.1"
                        :min="-360"
                        :max="360"
                        @update:model-value="(value) => updateModelTransformFast('SetRotation', 'y', value)"
                        @change="() => updateModelTransform('SetRotation')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.rotation.z"
                        :step="0.1"
                        :min="-360"
                        :max="360"
                        @update:model-value="(value) => updateModelTransformFast('SetRotation', 'z', value)"
                        @change="() => updateModelTransform('SetRotation')"
                      />
                    </div>
                  </div>
                </div>

                <!-- 缩放 -->
                <div>
                  <div class="text-[#909090] text-[10px] mb-0.5">缩放</div>
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.scale.x"
                        :step="0.1"
                        :min="0.01"
                        :max="10"
                        @update:model-value="(value) => updateModelTransformFast('SetScale', 'x', value)"
                        @change="() => updateModelTransform('SetScale')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.scale.y"
                        :step="0.1"
                        :min="0.01"
                        :max="10"
                        @update:model-value="(value) => updateModelTransformFast('SetScale', 'y', value)"
                        @change="() => updateModelTransform('SetScale')"
                      />
                    </div>

                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <NumberInputWithSlider
                        v-model="modelData.defaultTransform.scale.z"
                        :step="0.1"
                        :min="0.01"
                        :max="10"
                        @update:model-value="(value) => updateModelTransformFast('SetScale', 'z', value)"
                        @change="() => updateModelTransform('SetScale')"
                      />
                    </div>
                  </div>
                </div>
              </div>

              <!-- 碰撞选项 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-orange-600">
                <div class="flex items-center justify-between">
                  <label class="text-[#e0e0e0] font-medium">碰撞</label>
                  <div class="flex items-center space-x-4">
                    <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                      <input
                        v-model="modelData.collision.type"
                        type="radio"
                        value="none"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                        @change="updateModelCollision"
                        @update:model-value="updateModelCollisionFast"
                      />
                      <span>无</span>
                    </label>
                    <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                      <input
                        v-model="modelData.collision.type"
                        type="radio"
                        value="box"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                        @change="updateModelCollision"
                        @update:model-value="updateModelCollisionFast"
                      />
                      <span>包围盒</span>
                    </label>
                    <label class="text-[#909090] text-[10px] flex items-center space-x-1">
                      <input
                        v-model="modelData.collision.type"
                        type="radio"
                        value="mesh"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-[#84a65b]"
                        @change="updateModelCollision"
                        @update:model-value="updateModelCollisionFast"
                      />
                      <span>网格</span>
                    </label>
                  </div>
                </div>
              </div>

              <!-- 物理属性 -->
              <div class="bg-[#3c3c3c]/50 p-2 rounded border-l-2 border-purple-600">
                <label class="text-[#e0e0e0] font-medium block mb-1 text-[10px]">物理</label>
                <!-- 启用物理开关 -->
                <div class="mb-1 flex items-center justify-between">
                  <span class="text-[#909090] text-[10px]">启用物理</span>
                  <label class="relative inline-flex items-center cursor-pointer">
                    <input
                      type="checkbox"
                      v-model="modelData.mechanics.physics_enabled"
                      class="sr-only"
                      @change="() => updateModelMechanics('SetPhysicsEnabled')"
                      @update:model-value="() => updateModelMechanicsFast(PROPERTY.PhysicsEnabled)"
                    />
                    <div
                      class="relative w-7 h-4 rounded-full transition-colors"
                      :class="modelData.mechanics.physics_enabled ? 'bg-[#84a65b]/60' : 'bg-[#1a1a1a]'"
                    >
                      <span
                        class="absolute top-[2px] left-[2px] h-3 w-3 rounded-full transition-all"
                        :class="modelData.mechanics.physics_enabled ? 'translate-x-3 bg-[#84a65b]' : 'translate-x-0 bg-[#555]'"
                      ></span>
                    </div>
                  </label>
                </div>
                <!-- 质量 -->
                <div class="mb-1">
                  <div class="text-[#909090] text-[10px] mb-0.5">质量</div>
                  <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                    <NumberInputWithSlider
                      v-model="modelData.mechanics.mass"
                      :step="0.1"
                      :min="0.01"
                      :max="100"
                      @change="() => updateModelMechanics('SetMass')"
                      @update:model-value="() => updateModelMechanicsFast(PROPERTY.Mass)"
                    />
                  </div>
                </div>
                <!-- 弹性系数 -->
                <div class="mb-1">
                  <div class="text-[#909090] text-[10px] mb-0.5">弹性系数</div>
                  <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                    <NumberInputWithSlider
                      v-model="modelData.mechanics.restitution"
                      :step="0.05"
                      :min="0"
                      :max="1"
                      @change="() => updateModelMechanics('SetRestitution')"
                      @update:model-value="() => updateModelMechanicsFast(PROPERTY.Restitution)"
                    />
                  </div>
                </div>
                <!-- 阻尼 -->
                <div>
                  <div class="text-[#909090] text-[10px] mb-0.5">阻尼</div>
                  <div class="flex items-center bg-[#1a1a1a] rounded px-1 py-0.5">
                    <NumberInputWithSlider
                      v-model="modelData.mechanics.damping"
                      :step="0.01"
                      :min="0"
                      :max="1"
                      @change="() => updateModelMechanics('SetDamping')"
                      @update:model-value="() => updateModelMechanicsFast(PROPERTY.Damping)"
                    />
                  </div>
                </div>
                <!-- 轴锁定 - 平移 -->
                <div class="mt-2 pt-1 border-t border-[#1a1a1a]/50">
                  <div class="text-[#909090] text-[10px] mb-1">平移锁定</div>
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.linear_lock[0]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-red-500 w-3 h-3"
                        @change="() => { updateModelLinearLockFast(); updateModelMechanics('SetLinearLock'); }" />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.linear_lock[1]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-blue-500 w-3 h-3"
                        @change="() => { updateModelLinearLockFast(); updateModelMechanics('SetLinearLock'); }" />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.linear_lock[2]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-green-500 w-3 h-3"
                        @change="() => { updateModelLinearLockFast(); updateModelMechanics('SetLinearLock'); }" />
                    </div>
                  </div>
                </div>
                <!-- 轴锁定 - 旋转 -->
                <div class="mt-1">
                  <div class="text-[#909090] text-[10px] mb-1">旋转锁定</div>
                  <div class="grid grid-cols-3 gap-1">
                    <div class="flex items-center gap-1">
                      <label class="text-red-400 text-[10px] w-3">X</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.angular_lock[0]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-red-500 w-3 h-3"
                        @change="() => { updateModelAngularLockFast(); updateModelMechanics('SetAngularLock'); }" />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-blue-400 text-[10px] w-3">Y</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.angular_lock[1]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-blue-500 w-3 h-3"
                        @change="() => { updateModelAngularLockFast(); updateModelMechanics('SetAngularLock'); }" />
                    </div>
                    <div class="flex items-center gap-1">
                      <label class="text-green-400 text-[10px] w-3">Z</label>
                      <input type="checkbox"
                        v-model="modelData.mechanics.angular_lock[2]"
                        class="rounded bg-[#1a1a1a] border-[#3c3c3c] checked:bg-green-500 w-3 h-3"
                        @change="() => { updateModelAngularLockFast(); updateModelMechanics('SetAngularLock'); }" />
                    </div>
                  </div>
                </div>
              </div>
            </div>

            <!-- 模型 - 积木 -->
            <div v-show="ActiveSubTab === 'Blockly'" class="flex flex-col" style="height: 400px;">
              <BlocklyWorkspace
                v-if="modelData.name"
                ref="modelBlocklyRef"
                :actorName="modelData.name"
                :sceneName="modelData.targetScene || sceneData.name"
                embedded
              />
              <div v-else class="flex items-center justify-center h-full text-[#909090] text-xs">
                请先选中一个物体
              </div>
            </div>
          </template>
        </div>
      </template>
    </div>
  </div>
</template>

<script setup>
import NumberInputWithSlider from '@/components/ui/NumberInputWithSlider.vue';
import { ref, reactive, onMounted, onUnmounted, computed, watch, nextTick } from 'vue';
import { useRoute } from 'vue-router';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import BlocklyWorkspace from '@/blockly/components/BlocklyWorkspace.vue';
import { sceneService, projectService } from '@/utils/bridge.js';
import { DEFAULT_SCENE_NAME } from '@/utils/constants.js';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { setActorContext } from '@/blockly/composables/useActorContext.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import { useDockPanel } from '@/composables/useDockPanel.js';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();

const { error: logError, warn: logWarn } = useErrorHandler('Object');

// ========== 防抖工具 ==========
const _debounceTimers = {};
const debounced = (key, fn, delay = 200) => {
  clearTimeout(_debounceTimers[key]);
  _debounceTimers[key] = setTimeout(fn, delay);
};

// ========== 路由参数 ==========
const route = useRoute();

// ========== 主标签页状态 ==========
const mainActiveTab = ref('scene'); // 'scene', 'actor', 'model'
const ActiveSubTab = ref('Basic');

// ========== 标签页定义 ==========
const sceneTabs = [
  { id: 'Basic', label: '基础' },
  { id: 'Terrain', label: '地形' },
  { id: 'Script', label: '脚本' },
];

const actorTabs = [
  { id: 'Basic', label: '基础' },
  { id: 'Model', label: '模型' },
  { id: 'Blockly', label: '积木' },
  { id: 'Script', label: '脚本' },
];

const modelTabs = [
  { id: 'Basic', label: '基础' },
  { id: 'Model', label: '模型' },
  { id: 'Blockly', label: '积木' },
];

// ========== Blockly 工作区引用 ==========
const actorBlocklyRef = ref(null);
const modelBlocklyRef = ref(null);

// ========== 时间轴状态 ==========
const timelineRulerRef = ref(null);
const trackContentRef = ref(null);

const timelineState = reactive({
  playing: false,
  currentTime: 0,
  currentFrame: 0,
  totalFrames: 300,
  duration: 10.0,
  fps: 30,
  selectedTrack: 'track-anim',
});

const timelineTracks = reactive([
  { id: 'track-anim', name: '动画轨道', type: 'animation', icon: '🎬', expanded: true, clips: [], keyframes: [] },
  { id: 'track-audio', name: '音频轨道', type: 'audio', icon: '🔊', expanded: true, clips: [], keyframes: [] },
  { id: 'track-video', name: '视频轨道', type: 'video', icon: '🎥', expanded: false, clips: [], keyframes: [] },
  { id: 'track-keyframe', name: '关键帧轨道', type: 'keyframe', icon: '🔑', expanded: true, clips: [], keyframes: [] },
]);

// ── 播放控制 ──
let playbackRafId = null;
let lastPlaybackTime = 0;

function startPlayback() {
  timelineState.playing = true;
  lastPlaybackTime = performance.now();
  playbackRafId = requestAnimationFrame(playbackLoop);
}

function stopPlayback() {
  timelineState.playing = false;
  if (playbackRafId != null) {
    cancelAnimationFrame(playbackRafId);
    playbackRafId = null;
  }
}

function playbackLoop(now) {
  if (!timelineState.playing) return;
  const dt = (now - lastPlaybackTime) / 1000;
  lastPlaybackTime = now;
  timelineState.currentTime += dt;
  timelineState.currentFrame = Math.round(timelineState.currentTime * timelineState.fps);
  if (timelineState.currentFrame >= timelineState.totalFrames) {
    timelineState.currentFrame = timelineState.totalFrames;
    timelineState.currentTime = timelineState.duration;
    stopPlayback();
    return;
  }
  playbackRafId = requestAnimationFrame(playbackLoop);
}

function togglePlayback() {
  if (timelineState.playing) {
    stopPlayback();
  } else {
    if (timelineState.currentFrame >= timelineState.totalFrames) {
      timelineState.currentFrame = 0;
      timelineState.currentTime = 0;
    }
    startPlayback();
  }
}

function seekToStart() {
  stopPlayback();
  timelineState.currentFrame = 0;
  timelineState.currentTime = 0;
}

function seekToEnd() {
  stopPlayback();
  timelineState.currentFrame = timelineState.totalFrames;
  timelineState.currentTime = timelineState.duration;
}

// ── 一键清除 ──
function clearAllTimeline() {
  stopPlayback();
  timelineState.currentFrame = 0;
  timelineState.currentTime = 0;
  for (const track of timelineTracks) {
    track.clips.splice(0);
    track.keyframes.splice(0);
  }
}

// ── 片段长按拖拽创建 ──
const clipDragState = reactive({
  active: false,
  trackId: null,
  startFrame: 0,
  currentFrame: 0,
});

let longPressTimer = null;

function frameFromEvent(event) {
  const el = trackContentRef.value;
  if (!el) return 0;
  const rect = el.getBoundingClientRect();
  const x = event.clientX - rect.left;
  return Math.round((x / rect.width) * timelineState.totalFrames);
}

function onTrackMouseDown(track, event) {
  if (event.button !== 0) return;
  // 关键帧轨道：不触发长按拖拽（走 click 添加关键帧）
  if (track.type === 'keyframe') return;
  const frame = frameFromEvent(event);
  longPressTimer = setTimeout(() => {
    clipDragState.active = true;
    clipDragState.trackId = track.id;
    clipDragState.startFrame = frame;
    clipDragState.currentFrame = frame;
    longPressTimer = null;
  }, 300);
}

function onTrackMouseMove(track, event) {
  if (!clipDragState.active || clipDragState.trackId !== track.id) return;
  const frame = frameFromEvent(event);
  clipDragState.currentFrame = Math.max(0, Math.min(frame, timelineState.totalFrames));
}

function onTrackMouseUp(track, event) {
  if (longPressTimer) {
    clearTimeout(longPressTimer);
    longPressTimer = null;
  }
  if (clipDragState.active && clipDragState.trackId === track.id) {
    const s = clipDragState.startFrame;
    const e = clipDragState.currentFrame;
    const start = Math.min(s, e);
    const end = Math.max(s, e);
    const duration = end - start;
    if (duration > 1) {
      const count = track.clips.filter(c => c.startFrame === start).length + 1;
      track.clips.push({
        name: `${track.name}_${track.clips.length + 1}`,
        startFrame: start,
        durationFrames: duration,
      });
    }
    clipDragState.active = false;
    clipDragState.trackId = null;
  }
}

function onTrackMouseLeave(track, event) {
  if (longPressTimer) {
    clearTimeout(longPressTimer);
    longPressTimer = null;
  }
  if (clipDragState.active && clipDragState.trackId === track.id) {
    // 取消拖拽
    clipDragState.active = false;
    clipDragState.trackId = null;
  }
}

// ── 关键帧点击添加 ──
function onTrackClick(track, event) {
  if (track.type !== 'keyframe') return;
  // 如果刚结束拖拽，忽略 click
  const frame = frameFromEvent(event);
  if (frame < 0 || frame > timelineState.totalFrames) return;
  track.keyframes.push({
    frame: frame,
    value: `key_${frame}`,
  });
}

// ── 双击删除 ──
function deleteClip(track, index) {
  track.clips.splice(index, 1);
}

function deleteKeyframe(track, index) {
  track.keyframes.splice(index, 1);
}

// ── 标尺点击跳转 ──
function onRulerClick(event) {
  stopPlayback();
  const frame = frameFromEvent(event);
  timelineState.currentFrame = Math.max(0, Math.min(frame, timelineState.totalFrames));
  timelineState.currentTime = timelineState.currentFrame / timelineState.fps;
}

// ── 拖拽预览样式 ──
const clipDragPreviewStyle = computed(() => {
  const s = clipDragState.startFrame;
  const e = clipDragState.currentFrame;
  const start = Math.min(s, e);
  const end = Math.max(s, e);
  return {
    left: clipTimeToPercent(start) + '%',
    width: Math.max(clipTimeToPercent(end) - clipTimeToPercent(start), 1) + '%',
  };
});

function clipPreviewColor(type) {
  switch (type) {
    case 'animation': return 'bg-blue-500/40 border border-dashed border-blue-300/60';
    case 'audio': return 'bg-green-500/40 border border-dashed border-green-300/60';
    case 'video': return 'bg-purple-500/40 border border-dashed border-purple-300/60';
    default: return 'bg-gray-500/40 border border-dashed border-gray-300/60';
  }
}

const timelineTicks = computed(() => {
  const ticks = [];
  const totalFrames = timelineState.totalFrames;
  const fps = timelineState.fps;
  const totalSeconds = totalFrames / fps;
  for (let t = 0; t <= totalSeconds + 0.001; t += 0.5) {
    const isMajor = Math.abs(Math.round(t) - t) < 0.001;
    ticks.push({ time: t, left: (t / totalSeconds) * 100, major: isMajor, label: isMajor ? formatTimelineTime(t) : '' });
  }
  return ticks;
});

const playheadPosition = computed(() => {
  if (timelineState.totalFrames <= 0) return 0;
  return (timelineState.currentFrame / timelineState.totalFrames) * 100;
});

function formatTimelineTime(seconds) {
  const t = Math.max(0, seconds);
  const m = Math.floor(t / 60);
  const s = Math.floor(t % 60);
  const ms = Math.floor((t % 1) * 1000);
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}.${String(ms).padStart(3, '0')}`;
}

function clipTimeToPercent(frame) {
  if (timelineState.totalFrames <= 0) return 0;
  return (frame / timelineState.totalFrames) * 100;
}

function clipColorClass(type) {
  switch (type) {
    case 'animation': return 'bg-blue-600/70 border border-blue-400/50 hover:bg-blue-500/80';
    case 'audio': return 'bg-green-700/70 border border-green-500/50 hover:bg-green-600/80';
    case 'video': return 'bg-purple-700/70 border border-purple-500/50 hover:bg-purple-600/80';
    case 'keyframe': return 'bg-yellow-600/50 border border-yellow-500/30 hover:bg-yellow-500/60';
    default: return 'bg-gray-600/70 border border-gray-500/50';
  }
}

function keyframeColorClass(type) {
  switch (type) {
    case 'animation': return 'bg-blue-400 border border-blue-200';
    case 'audio': return 'bg-green-400 border border-green-200';
    case 'video': return 'bg-purple-400 border border-purple-200';
    case 'keyframe': return 'bg-yellow-400 border border-yellow-200';
    default: return 'bg-gray-400 border border-gray-200';
  }
}

// ========== 当前打开的文件 ==========
const currentActorFile = ref('');
const currentModelFile = ref('');
const actorAliasDraft = ref('');
const actorAliasSaving = ref(false);
const actorAliasError = ref('');
const modelAliasDraft = ref('');
const modelAliasSaving = ref(false);
const modelAliasError = ref('');

// ========== 场景数据 ==========
const sceneData = ref({
  sceneId: '',
  name: '默认场景',
  activeCameraName: null,
  cameras: [],
  light: {
    enabled: true,
    direction: { x: 1.0, y: 1.0, z: 1.0 },
  },
  terrain: {
    type: 'none',
    path: '',
    size: 10.0,
  },
  grid: {
    enabled: true,
  },
  script: {
    path: '',
  },
});

// ========== 单位数据 ==========
const actorData = ref({
  name: '',
  handle: 0,
  parentScene: '',
  file: '',
  follow_camera: false,
  render_space: 'scene',
  model: {
    path: '',
  },

  hasGeometry: false,
  transform: {
    position: { x: 0.0, y: 0.0, z: 0.0 },
    rotation: { x: 0.0, y: 0.0, z: 0.0 },
    scale: { x: 1.0, y: 1.0, z: 1.0 },
  },
  collision: {
    type: 'none',
  },

  hasMechanics: false,
  mechanics: {
    mass: 1.0,
    restitution: 0.8,
    damping: 0.99,
    physics_enabled: true,
    linear_lock: [false, false, false],
    angular_lock: [false, false, false],
  },
  script: {
    path: '',
  },
  camera_lock: {
    lock_to_camera: false,
    position_offset: { x: 0.0, y: 0.0, z: 2.0 },
    rotation_offset: { x: 0.0, y: 0.0, z: 0.0 },
  },
});

// ========== 模型数据 ==========
const modelData = ref({
  name: '',
  handle: 0,
  targetScene: '',
  file: '',
  follow_camera: false,
  render_space: 'scene',
  defaultTransform: {
    position: { x: 0.0, y: 0.0, z: 0.0 },
    rotation: { x: 0.0, y: 0.0, z: 0.0 },
    scale: { x: 1.0, y: 1.0, z: 1.0 },
  },
  collision: {
    type: 'none',
  },
  mechanics: {
    mass: 1.0,
    restitution: 0.8,
    damping: 0.99,
    physics_enabled: true,
    linear_lock: [false, false, false],
    angular_lock: [false, false, false],
  },
});

// ========== 计算属性 ==========
const actorAliasDirty = computed(() =>
  actorAliasDraft.value.trim() !== (actorData.value.name || '')
);
const modelAliasDirty = computed(() =>
  modelAliasDraft.value.trim() !== (modelData.value.name || '')
);

const currentFileInfo = computed(() => {
  if (mainActiveTab.value === 'scene') {
    return `🎬 场景: ${sceneData.value.name}`;
  } else if (mainActiveTab.value === 'actor') {
    const fileName = currentActorFile.value ? actorData.value.name : '未打开';
    return `👤 单位: ${fileName}`;
  } else if (mainActiveTab.value === 'timeline') {
    return `⏱ 时间轴: ${formatTimelineTime(timelineState.currentTime)} / ${formatTimelineTime(timelineState.duration)}`;
  } else {
    const fileName = currentModelFile.value ? modelData.value.name : '未打开';
    return `📦 模型: ${fileName}`;
  }
});

// ========== 方法 ==========
const switchMainTab = (tab) => {
  mainActiveTab.value = tab;
  ActiveSubTab.value = 'Basic'; // 重置子标签
};

const readFollowCameraState = (data) =>
  data?.render_space === 'ui' ||
  data?.follow_camera === true ||
  data?.follow_camera === 1 ||
  data?.follow_camera === 'true' ||
  data?.follow_camera === '1';

const unwrapBridgeResult = (result) => result?.data ?? result;

const resetActorAliasDraft = () => {
  actorAliasDraft.value = actorData.value.name || '';
  actorAliasError.value = '';
};

const commitActorAlias = async () => {
  const sceneName = actorData.value.parentScene;
  const actorName = currentActorFile.value || actorData.value.name;
  const nextName = actorAliasDraft.value.trim();

  if (!sceneName || !actorName || actorAliasSaving.value) return;
  if (!nextName) {
    actorAliasError.value = '别名不能为空';
    return;
  }
  if (nextName === actorData.value.name) {
    resetActorAliasDraft();
    return;
  }

  actorAliasSaving.value = true;
  actorAliasError.value = '';
  try {
    const result = unwrapBridgeResult(
      await sceneService.renameActor(sceneName, actorName, nextName)
    );
    if (result?.status === 'error') {
      throw new Error(result.message || '修改别名失败');
    }

    const savedName = result?.actor?.name || result?.new_name || nextName;
    currentActorFile.value = savedName;
    actorData.value.name = savedName;
    actorAliasDraft.value = savedName;
    setActorContext(sceneName, savedName);
  } catch (e) {
    actorAliasError.value = e?.message || '修改别名失败';
    actorAliasDraft.value = actorData.value.name || actorName;
    logError('修改单位别名失败', e);
  } finally {
    actorAliasSaving.value = false;
  }
};

const resetModelAliasDraft = () => {
  modelAliasDraft.value = modelData.value.name || '';
  modelAliasError.value = '';
};

const commitModelAlias = async () => {
  const sceneName = modelData.value.targetScene;
  const actorName = currentModelFile.value || modelData.value.name;
  const nextName = modelAliasDraft.value.trim();

  if (!sceneName || !actorName || modelAliasSaving.value) return;
  if (!nextName) {
    modelAliasError.value = '别名不能为空';
    return;
  }
  if (nextName === modelData.value.name) {
    resetModelAliasDraft();
    return;
  }

  modelAliasSaving.value = true;
  modelAliasError.value = '';
  try {
    const result = unwrapBridgeResult(
      await sceneService.renameActor(sceneName, actorName, nextName)
    );
    if (result?.status === 'error') {
      throw new Error(result.message || '修改别名失败');
    }

    const savedName = result?.actor?.name || result?.new_name || nextName;
    currentModelFile.value = savedName;
    modelData.value.name = savedName;
    modelAliasDraft.value = savedName;
    setActorContext(sceneName, savedName);
  } catch (e) {
    modelAliasError.value = e?.message || '修改别名失败';
    modelAliasDraft.value = modelData.value.name || actorName;
    logError('修改模型别名失败', e);
  } finally {
    modelAliasSaving.value = false;
  }
};

// 加载场景数据
const loadSceneData = async (sceneId) => {
  if (!sceneId) return;

  try {
    // 调用后端接口获取场景数据
    const result = await sceneService.getScene(sceneId);
    if (result) {
      const data = result.data;

      sceneData.value.sceneId = data.scene_id || data.id || sceneId;
      sceneData.value.name = data.name || sceneId;
      sceneData.value.activeCameraName = data.active_camera_name || null;
      sceneData.value.cameras = Array.isArray(data.cameras) ? data.cameras : [];

      // 加载光照数据
      if (data.sun) {
        sceneData.value.light.enabled = data.sun.enabled !== false;
        sceneData.value.light.direction = {
          x: data.sun.direction?.[0] || 1.0,
          y: data.sun.direction?.[1] || 1.0,
          z: data.sun.direction?.[2] || 1.0,
        };
      }

      // 加载地形数据
      if (data.terrain) {
        sceneData.value.terrain.type = data.terrain.type || 'none';
        sceneData.value.terrain.path = data.terrain.path || '';
        sceneData.value.terrain.size = data.terrain.size || 10.0;
      }

      // 加载网格数据（兼容不同后端字段名）
      if (data.grid && typeof data.grid.enabled === 'boolean') {
        sceneData.value.grid.enabled = data.grid.enabled;
      } else if (typeof data.floor_grid_enabled === 'boolean') {
        sceneData.value.grid.enabled = data.floor_grid_enabled;
      }

      // 加载脚本数据
      sceneData.value.script.path = data.script || '';
    }
  } catch (e) {
    logError('加载场景数据失败', e);
  }
};

// 加载单位数据 - 需要场景ID和单位ID
const loadActorData = async (sceneId, actorId) => {
  if (!actorId) return;

  try {
    // 调用后端接口获取单位数据，传入场景ID和单位ID
    const result = await sceneService.getActor(sceneId, actorId);
    if (result) {
      const data = result.data;
      const displayName = data.name || actorId;
      actorData.value.name = displayName;
      actorAliasDraft.value = displayName;
      actorAliasError.value = '';
      actorData.value.handle = Number(data.handle || 0);
      actorData.value.parentScene = sceneId || '';
      actorData.value.file = data.path;
      actorData.value.model.path = data.model || '';
      const followCamera = readFollowCameraState(data);
      actorData.value.follow_camera = followCamera;
      actorData.value.render_space = followCamera ? 'ui' : 'scene';

      if (data.geometry) {
        actorData.value.hasGeometry = true;
        actorData.value.transform.position = {
          x: data.geometry.position?.[0] || 0,
          y: data.geometry.position?.[1] || 0,
          z: data.geometry.position?.[2] || 0,
        };
        actorData.value.transform.rotation = {
          x: data.geometry.rotation?.[0] || 0,
          y: data.geometry.rotation?.[1] || 0,
          z: data.geometry.rotation?.[2] || 0,
        };
        actorData.value.transform.scale = {
          x: data.geometry.scale?.[0] || 1,
          y: data.geometry.scale?.[1] || 1,
          z: data.geometry.scale?.[2] || 1,
        };
      } else {
        actorData.value.hasGeometry = false;
      }

      actorData.value.collision.type = data.collision || 'none';
      actorData.value.script.path = data.script || '';

      // 相机锁定数据
      if (data.camera_lock) {
        actorData.value.camera_lock.lock_to_camera =
          data.camera_lock.lock_to_camera || false;
        actorData.value.camera_lock.position_offset = {
          x: data.camera_lock.position_offset?.[0] || 0,
          y: data.camera_lock.position_offset?.[1] || 0,
          z: data.camera_lock.position_offset?.[2] || 2,
        };
        actorData.value.camera_lock.rotation_offset = {
          x: data.camera_lock.rotation_offset?.[0] || 0,
          y: data.camera_lock.rotation_offset?.[1] || 0,
          z: data.camera_lock.rotation_offset?.[2] || 0,
        };
      }

      if (data.mechanics) {
        actorData.value.hasMechanics = true;
        actorData.value.mechanics.mass = data.mechanics.mass ?? 1.0;
        actorData.value.mechanics.restitution = data.mechanics.restitution ?? 0.8;
        actorData.value.mechanics.damping = data.mechanics.damping ?? 0.99;
        actorData.value.mechanics.physics_enabled = data.mechanics.physics_enabled ?? true;
        if (data.mechanics.linear_lock !== undefined) {
          const ll = data.mechanics.linear_lock;
          actorData.value.mechanics.linear_lock = [Boolean(ll[0]), Boolean(ll[1]), Boolean(ll[2])];
        }
        if (data.mechanics.angular_lock !== undefined) {
          const al = data.mechanics.angular_lock;
          actorData.value.mechanics.angular_lock = [Boolean(al[0]), Boolean(al[1]), Boolean(al[2])];
        }
      } else {
        actorData.value.hasMechanics = false;
      }
    }
  } catch (e) {
    logError('加载单位数据失败', e);
  }
};

// 加载模型数据 - 需要场景ID和模型ID
const loadModelData = async (sceneId, modelId) => {
  if (!modelId) return;

  try {
    // 调用后端接口获取模型数据，传入场景ID和模型ID
    const result = await sceneService.getActor(sceneId, modelId);
    if (result) {
      const data = result.data;
      const displayName = data.name || modelId;
      modelData.value.name = displayName;
      modelAliasDraft.value = displayName;
      modelAliasError.value = '';
      modelData.value.handle = Number(data.handle || 0);
      modelData.value.targetScene = sceneId || '';
      modelData.value.file = data.path;
      const followCamera = readFollowCameraState(data);
      modelData.value.follow_camera = followCamera;
      modelData.value.render_space = followCamera ? 'ui' : 'scene';

      if (data.geometry) {
        modelData.value.defaultTransform.position = {
          x: data.geometry.position?.[0] || 0,
          y: data.geometry.position?.[1] || 0,
          z: data.geometry.position?.[2] || 0,
        };
        modelData.value.defaultTransform.rotation = {
          x: data.geometry.rotation?.[0] || 0,
          y: data.geometry.rotation?.[1] || 0,
          z: data.geometry.rotation?.[2] || 0,
        };
        modelData.value.defaultTransform.scale = {
          x: data.geometry.scale?.[0] || 1,
          y: data.geometry.scale?.[1] || 1,
          z: data.geometry.scale?.[2] || 1,
        };
      }

      modelData.value.collision.type = data.collision || 'none';

      if (data.mechanics) {
        modelData.value.mechanics.mass = data.mechanics.mass ?? 1.0;
        modelData.value.mechanics.restitution = data.mechanics.restitution ?? 0.8;
        modelData.value.mechanics.damping = data.mechanics.damping ?? 0.99;
        modelData.value.mechanics.physics_enabled = data.mechanics.physics_enabled ?? true;
        if (data.mechanics.linear_lock !== undefined) {
          const ll = data.mechanics.linear_lock;
          modelData.value.mechanics.linear_lock = [Boolean(ll[0]), Boolean(ll[1]), Boolean(ll[2])];
        }
        if (data.mechanics.angular_lock !== undefined) {
          const al = data.mechanics.angular_lock;
          modelData.value.mechanics.angular_lock = [Boolean(al[0]), Boolean(al[1]), Boolean(al[2])];
        }
      }
    }
  } catch (e) {
    logError('加载模型数据失败', e);
  }
};

// 更新光照方向
const updateLightDirection = async () => {
  if (mainActiveTab.value !== 'scene' || !sceneData.value.name) return;

  try {
    // 如果光照启用，发送实际方向值；如果禁用，发送 (0,0,0) 或 (0,-1,0) 等表示无光照
    const direction = {
      x: parseFloat(sceneData.value.light.direction.x),
      y: parseFloat(sceneData.value.light.direction.y),
      z: parseFloat(sceneData.value.light.direction.z),
    };
    await sceneService.sunDirection(
      sceneData.value.sceneId || sceneData.value.name,
      sceneData.value.light.enabled,
      [direction.x, direction.y, direction.z]
    );
  } catch (e) {
    logError('更新光照方向失败', e);
  }
};

const updateFloorGrid = async () => {
  if (mainActiveTab.value !== 'scene' || !sceneData.value.name) return;

  try {
    if (typeof sceneService.floorGrid === 'function') {
      await sceneService.floorGrid(
        sceneData.value.sceneId || sceneData.value.name,
        sceneData.value.grid.enabled
      );
    } else {
      logWarn('sceneService.floorGrid 未定义，暂无法同步网格开关到后端');
    }
  } catch (e) {
    logError('更新网格显示状态失败', e);
  }
};

const ACTOR_TRANSFORM_OPERATION = {
  SetPosition: 0,
  SetRotation: 1,
  SetScale: 2,
  Move: 0,
  Rotate: 1,
  Scale: 2,
};

// 属性编辑快速通道常量（与 cef_subprocess/main.cpp 中 setProperty 的 propertyType 一致）
const PROPERTY = {
  Mass: 0,
  Restitution: 1,
  Damping: 2,
  Visible: 3,
  CollisionEnabled: 4,
  PhysicsEnabled: 5,
  LinearLockMask: 6,
  AngularLockMask: 7,
};

const applyAxisOverride = (vector, axis, value) => {
  if (!axis) return vector;
  const axisIndex = { x: 0, y: 1, z: 2 }[axis];
  if (axisIndex === undefined) return vector;
  vector[axisIndex] = Number(value);
  return vector;
};

const getTransformVector = (transform, operationType, axis = null, value = null) => {
  if (!transform) return null;
  switch (operationType) {
    case 'SetPosition':
    case 'Move':
      return applyAxisOverride(
        [
          Number(transform.position.x),
          Number(transform.position.y),
          Number(transform.position.z),
        ],
        axis,
        value
      );
    case 'SetRotation':
    case 'Rotate':
      return applyAxisOverride(
        [
          Number(transform.rotation.x),
          Number(transform.rotation.y),
          Number(transform.rotation.z),
        ],
        axis,
        value
      );
    case 'SetScale':
    case 'Scale':
      return applyAxisOverride(
        [
          Number(transform.scale.x),
          Number(transform.scale.y),
          Number(transform.scale.z),
        ],
        axis,
        value
      );
    default:
      return null;
  }
};

const getActorTransformVector = (operationType, axis = null, value = null) =>
  getTransformVector(actorData.value.transform, operationType, axis, value);

const getModelTransformVector = (operationType, axis = null, value = null) =>
  getTransformVector(modelData.value.defaultTransform, operationType, axis, value);

const actorTransformFastWarnings = new Set();
const pendingTransformFastUpdates = new Map();
let transformFastRafId = null;

const warnActorTransformFastOnce = (key, message, detail = null) => {
  if (actorTransformFastWarnings.has(key)) return;
  actorTransformFastWarnings.add(key);
  logWarn(message, detail);
};

const flushTransformFastUpdates = () => {
  transformFastRafId = null;
  const updates = Array.from(pendingTransformFastUpdates.values());
  pendingTransformFastUpdates.clear();

  for (const update of updates) {
    try {
      update.bridge.actorTransform(update.handle, update.operation, update.vector);
    } catch (e) {
      logError('V8 更新物体变换失败', e);
    }
  }
};

const queueTransformFastUpdate = (update) => {
  const key = `${update.source}:${update.handle}:${update.operation}`;
  pendingTransformFastUpdates.set(key, update);
  if (transformFastRafId != null) return;
  transformFastRafId = requestAnimationFrame(flushTransformFastUpdates);
};

const updateTransformFast = ({ source, actorName, handle, operationType, axis = null, value = null, vector }) => {
  const operation = ACTOR_TRANSFORM_OPERATION[operationType];
  const bridge = window.coronaBridge;
  if (!handle) {
    warnActorTransformFastOnce(`${source}-missing-handle`, 'ActorTransformFast skipped: actor handle is missing', {
      source,
      actor: actorName,
      handle,
    });
    return;
  }
  if (operation === undefined || !vector) {
    warnActorTransformFastOnce(`${source}-bad-operation`, `ActorTransformFast skipped: invalid operation ${operationType}`, {
      source,
      actor: actorName,
      handle,
    });
    return;
  }
  if (!bridge || typeof bridge.actorTransform !== 'function') {
    warnActorTransformFastOnce(`${source}-missing-bridge`, 'ActorTransformFast skipped: window.coronaBridge.actorTransform is unavailable', {
      source,
      actor: actorName,
      handle,
    });
    return;
  }

  queueTransformFastUpdate({
    source,
    handle,
    operation,
    vector,
    bridge,
  });
};

const updateActorTransformFast = (operationType, axis = null, value = null) => {
  updateTransformFast({
    source: 'actor',
    actorName: currentActorFile.value,
    handle: actorData.value.handle,
    operationType,
    axis,
    value,
    vector: getActorTransformVector(operationType, axis, value),
  });
};

const updateModelTransformFast = (operationType, axis = null, value = null) => {
  updateTransformFast({
    source: 'model',
    actorName: currentModelFile.value,
    handle: modelData.value.handle,
    operationType,
    axis,
    value,
    vector: getModelTransformVector(operationType, axis, value),
  });
};

// 更新单位变换——快速通道已写入 SharedDataHub，此处仅触发写盘
const updateActorTransform = (operationType) => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  debounced(`actor_transform_${operationType}`, async () => {
    try {
      await sceneService.saveActor(
        actorData.value.parentScene,
        currentActorFile.value
      );
    } catch (e) {
      logError('更新单位变换失败', e);
    }
  });
};

// 更新单位物理属性 — 快速通道写入 SharedDataHub
const updateActorMechanicsFast = (propertyType) => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!actorData.value.handle) return;
  let value = 0;
  switch (propertyType) {
    case PROPERTY.Mass: value = actorData.value.mechanics.mass; break;
    case PROPERTY.Restitution: value = actorData.value.mechanics.restitution; break;
    case PROPERTY.Damping: value = actorData.value.mechanics.damping; break;
    case PROPERTY.PhysicsEnabled: value = actorData.value.mechanics.physics_enabled ? 1 : 0; break;
    default: return;
  }
  try { bridge.setProperty(actorData.value.handle, propertyType, value); } catch (e) { /* ignore */ }
};

// ── 轴锁定快速通道 ──
const computeLockMask = (lock_array) =>
  (lock_array[0] ? 1 : 0) | (lock_array[1] ? 2 : 0) | (lock_array[2] ? 4 : 0);

const updateLinearLockFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!actorData.value.handle) return;
  const mask = computeLockMask(actorData.value.mechanics.linear_lock);
  try { bridge.setProperty(actorData.value.handle, PROPERTY.LinearLockMask, mask); } catch (e) {}
};

const updateAngularLockFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!actorData.value.handle) return;
  const mask = computeLockMask(actorData.value.mechanics.angular_lock);
  try { bridge.setProperty(actorData.value.handle, PROPERTY.AngularLockMask, mask); } catch (e) {}
};

const updateModelLinearLockFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!modelData.value.handle) return;
  const mask = computeLockMask(modelData.value.mechanics.linear_lock);
  try { bridge.setProperty(modelData.value.handle, PROPERTY.LinearLockMask, mask); } catch (e) {}
};

const updateModelAngularLockFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!modelData.value.handle) return;
  const mask = computeLockMask(modelData.value.mechanics.angular_lock);
  try { bridge.setProperty(modelData.value.handle, PROPERTY.AngularLockMask, mask); } catch (e) {}
};

// 更新单位物理属性 — 慢通道：触发 Python 写盘 + 设置
const updateActorMechanics = (operationType) => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  debounced(`actor_mechanics_${operationType}`, async () => {
    try {
      let value = 0;
      switch (operationType) {
        case 'SetMass':
          value = actorData.value.mechanics.mass;
          break;
        case 'SetRestitution':
          value = actorData.value.mechanics.restitution;
          break;
        case 'SetDamping':
          value = actorData.value.mechanics.damping;
          break;
        case 'SetPhysicsEnabled':
          value = actorData.value.mechanics.physics_enabled;
          break;
        case 'SetLinearLock':
          await sceneService.actorOperation(
            actorData.value.parentScene,
            currentActorFile.value,
            'SetLinearLock',
            actorData.value.mechanics.linear_lock.map(v => v ? 1 : 0)
          );
          return;
        case 'SetAngularLock':
          await sceneService.actorOperation(
            actorData.value.parentScene,
            currentActorFile.value,
            'SetAngularLock',
            actorData.value.mechanics.angular_lock.map(v => v ? 1 : 0)
          );
          return;
        default:
          return;
      }

      await sceneService.actorOperation(
        actorData.value.parentScene,
        currentActorFile.value,
        operationType,
        [value]
      );
    } catch (e) {
      logError('更新单位物理属性失败', e);
    }
  });
};

// 更新单位碰撞类型 — 快速通道写入 SharedDataHub
const updateActorCollisionFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!actorData.value.handle) return;
  const value = actorData.value.collision.type !== 'none' ? 1 : 0;
  try { bridge.setProperty(actorData.value.handle, PROPERTY.CollisionEnabled, value); } catch (e) {}
};

// 更新单位碰撞类型 — 慢通道：触发 Python 写盘 + 设置
const updateActorCollision = () => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  debounced('actor_collision', async () => {
    try {
      await sceneService.actorOperation(
        actorData.value.parentScene,
        currentActorFile.value,
        'SetCollision',
        [actorData.value.collision.type]
      );
    } catch (e) {
      logError('更新单位碰撞类型失败', e);
    }
  });
};

// 更新单位渲染空间：场景 Actor 或屏幕 UI Actor
const setActorRenderSpace = async (followCamera) => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;

  const enabled = Boolean(followCamera);
  const previous = Boolean(actorData.value.follow_camera);
  const previousPhysicsEnabled = Boolean(actorData.value.mechanics.physics_enabled);
  if (previous === enabled) return;

  actorData.value.follow_camera = enabled;
  actorData.value.render_space = enabled ? 'ui' : 'scene';

  try {
    await sceneService.actorOperation(
      actorData.value.parentScene,
      currentActorFile.value,
      'SetFollowCamera',
      [enabled]
    );
    if (enabled) {
      actorData.value.mechanics.physics_enabled = false;
    }
  } catch (e) {
    actorData.value.follow_camera = previous;
    actorData.value.render_space = previous ? 'ui' : 'scene';
    actorData.value.mechanics.physics_enabled = previousPhysicsEnabled;
    logError('更新单位渲染空间失败', e);
  }
};

// 更新相机锁定
const updateCameraLock = () => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  debounced('camera_lock_toggle', async () => {
    try {
      await sceneService.setCameraLock(
        actorData.value.parentScene,
        currentActorFile.value,
        actorData.value.camera_lock.lock_to_camera
      );
    } catch (e) {
      logError('更新相机锁定状态失败', e);
    }
  });
};

const updateCameraLockOffset = () => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  if (!actorData.value.camera_lock.lock_to_camera) return;
  debounced('camera_lock_offset', async () => {
    try {
      const offset = [
        Number(actorData.value.camera_lock.position_offset.x),
        Number(actorData.value.camera_lock.position_offset.y),
        Number(actorData.value.camera_lock.position_offset.z),
      ];
      await sceneService.setCameraLockOffset(
        actorData.value.parentScene,
        currentActorFile.value,
        offset
      );
    } catch (e) {
      logError('更新相机位置偏移失败', e);
    }
  });
};

const updateCameraLockRotation = () => {
  if (!currentActorFile.value || !actorData.value.parentScene) return;
  if (!actorData.value.camera_lock.lock_to_camera) return;
  debounced('camera_lock_rotation', async () => {
    try {
      const rotation = [
        Number(actorData.value.camera_lock.rotation_offset.x),
        Number(actorData.value.camera_lock.rotation_offset.y),
        Number(actorData.value.camera_lock.rotation_offset.z),
      ];
      await sceneService.setCameraLockRotation(
        actorData.value.parentScene,
        currentActorFile.value,
        rotation
      );
    } catch (e) {
      logError('更新相机旋转偏移失败', e);
    }
  });
};

// 更新模型变换——快速通道已写入 SharedDataHub，此处仅触发写盘
const updateModelTransform = (operationType) => {
  if (!currentModelFile.value || !modelData.value.targetScene) return;
  debounced(`model_transform_${operationType}`, async () => {
    try {
      await sceneService.saveActor(
        modelData.value.targetScene,
        currentModelFile.value
      );
    } catch (e) {
      logError('更新模型变换失败', e);
    }
  });
};

// 更新模型渲染空间：场景 Actor 或屏幕 UI Actor
const setModelRenderSpace = async (followCamera) => {
  if (!currentModelFile.value || !modelData.value.targetScene) return;

  const enabled = Boolean(followCamera);
  const previous = Boolean(modelData.value.follow_camera);
  const previousPhysicsEnabled = Boolean(modelData.value.mechanics.physics_enabled);
  if (previous === enabled) return;

  modelData.value.follow_camera = enabled;
  modelData.value.render_space = enabled ? 'ui' : 'scene';

  try {
    await sceneService.actorOperation(
      modelData.value.targetScene,
      currentModelFile.value,
      'SetFollowCamera',
      [enabled]
    );
    if (enabled) {
      modelData.value.mechanics.physics_enabled = false;
    }
  } catch (e) {
    modelData.value.follow_camera = previous;
    modelData.value.render_space = previous ? 'ui' : 'scene';
    modelData.value.mechanics.physics_enabled = previousPhysicsEnabled;
    logError('更新模型渲染空间失败', e);
  }
};

// 更新模型物理属性
// 更新模型物理属性 — 快速通道写入 SharedDataHub
const updateModelMechanicsFast = (propertyType) => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!modelData.value.handle) return;
  let value = 0;
  switch (propertyType) {
    case PROPERTY.Mass: value = modelData.value.mechanics.mass; break;
    case PROPERTY.Restitution: value = modelData.value.mechanics.restitution; break;
    case PROPERTY.Damping: value = modelData.value.mechanics.damping; break;
    case PROPERTY.PhysicsEnabled: value = modelData.value.mechanics.physics_enabled ? 1 : 0; break;
    default: return;
  }
  try { bridge.setProperty(modelData.value.handle, propertyType, value); } catch (e) {}
};

const updateModelMechanics = (operationType) => {
  if (!currentModelFile.value || !modelData.value.targetScene) return;
  debounced(`model_mechanics_${operationType}`, async () => {
    try {
      let value = 0;
      switch (operationType) {
        case 'SetMass':
          value = modelData.value.mechanics.mass;
          break;
        case 'SetRestitution':
          value = modelData.value.mechanics.restitution;
          break;
        case 'SetDamping':
          value = modelData.value.mechanics.damping;
          break;
        case 'SetPhysicsEnabled':
          value = modelData.value.mechanics.physics_enabled;
          break;
        case 'SetLinearLock':
          await sceneService.actorOperation(
            modelData.value.targetScene,
            currentModelFile.value,
            'SetLinearLock',
            modelData.value.mechanics.linear_lock.map(v => v ? 1 : 0)
          );
          return;
        case 'SetAngularLock':
          await sceneService.actorOperation(
            modelData.value.targetScene,
            currentModelFile.value,
            'SetAngularLock',
            modelData.value.mechanics.angular_lock.map(v => v ? 1 : 0)
          );
          return;
        default:
          return;
      }

      await sceneService.actorOperation(
        modelData.value.targetScene,
        currentModelFile.value,
        operationType,
        [value]
      );
    } catch (e) {
      logError('更新模型物理属性失败', e);
    }
  });
};

// 更新模型碰撞类型 — 快速通道写入 SharedDataHub
const updateModelCollisionFast = () => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.setProperty !== 'function') return;
  if (!modelData.value.handle) return;
  const value = modelData.value.collision.type !== 'none' ? 1 : 0;
  try { bridge.setProperty(modelData.value.handle, PROPERTY.CollisionEnabled, value); } catch (e) {}
};

// 更新模型碰撞类型
const updateModelCollision = () => {
  if (!currentModelFile.value || !modelData.value.targetScene) return;
  debounced('model_collision', async () => {
    try {
      await sceneService.actorOperation(
        modelData.value.targetScene,
        currentModelFile.value,
        'SetCollision',
        [modelData.value.collision.type]
      );
    } catch (e) {
      logError('更新模型碰撞类型失败', e);
    }
  });
};

// 文件选择方法
const selectTerrainFile = async () => {
  try {
    const result = await sceneService.selectModelFileDialog(
      sceneData.value.sceneId || sceneData.value.name,
      '',
      'terrain'
    );
    if (result.success && result.data) {
      sceneData.value.terrain.path = result.data;
    }
  } catch (e) {
    logError('选择地形文件失败', e);
  }
};

const selectSceneScript = async () => {
  try {
    const result = await sceneService.selectModelFileDialog(
      sceneData.value.sceneId || sceneData.value.name,
      '',
      'script'
    );
    if (result.success && result.data) {
      sceneData.value.script.path = result.data;
    }
  } catch (e) {
    logError('选择场景脚本失败', e);
  }
};

const selectModelFile = async () => {
  if (mainActiveTab.value !== 'actor') return;
  try {
    const result = await sceneService.selectModelFileDialog(
      actorData.value.parentScene,
      actorData.value.name,
      'model'
    );
    if (result.success && result.data) {
      actorData.value.model.path = result.data;
    }
  } catch (e) {
    logError('选择模型文件失败', e);
  }
};

const selectActorScript = async () => {
  try {
    const result = await sceneService.selectModelFileDialog(
      actorData.value.parentScene,
      actorData.value.name,
      'script'
    );
    if (result.success && result.data) {
      actorData.value.script.path = result.data;
    }
  } catch (e) {
    logError('选择单位脚本失败', e);
  }
};

const CloseFloat = async () => {
  if (closeDockPanel) { closeDockPanel(); return; }
};

const onBlocklyResize = () => {
  // Blockly调整大小处理
};

// ========== 变换更新回调函数 ==========
const onActorTransformUpdated = (position, rotation, scale) => {
  // 只在当前标签页是 actor 时才更新
  if (mainActiveTab.value !== 'actor') return;

  // 更新 UI 数据
  actorData.value.transform.position = {
    x: position[0],
    y: position[1],
    z: position[2],
  };
  actorData.value.transform.rotation = {
    x: rotation[0],
    y: rotation[1],
    z: rotation[2],
  };
  actorData.value.transform.scale = {
    x: scale[0],
    y: scale[1],
    z: scale[2],
  };
};

const onModelTransformUpdated = (position, rotation, scale) => {
  // 只在当前标签页是 model 时才更新
  if (mainActiveTab.value !== 'model') return;

  modelData.value.defaultTransform.position = {
    x: position[0],
    y: position[1],
    z: position[2],
  };
  modelData.value.defaultTransform.rotation = {
    x: rotation[0],
    y: rotation[1],
    z: rotation[2],
  };
  modelData.value.defaultTransform.scale = {
    x: scale[0],
    y: scale[1],
    z: scale[2],
  };
};

// ========== 事件监听 ==========
const setupWindowListener = () => {
  // 监听文件打开事件
  // 参数: type (scene/actor/model), scene_id, actor_id, old_path (可选，重命名时传入)
  window.onActorChange = async (type, scene_id, actor_id, old_path) => {
    if (!type) return;

    // 判断是否是重命名操作（有old_path参数）
    const isRename = old_path !== undefined;

    if (type === 'scene') {
      // --- 通知积木编辑器：切换了场景，清空 actor 上下文 ---
      setActorContext(scene_id, '');

      // 如果是重命名，检查当前是否正在编辑该文件
      if (isRename) {
        if (old_path === (sceneData.value.sceneId || sceneData.value.name)) {
          await loadSceneData(scene_id);
          ActiveSubTab.value = 'Basic';
        }
      }
      // 打开文件操作，直接切换到场景标签并加载
      else {
        mainActiveTab.value = 'scene';
        await loadSceneData(scene_id);
        ActiveSubTab.value = 'Basic';
      }
    } else if (type === 'actor') {
      // --- 通知积木编辑器：选中了场景中的 Actor ---
      setActorContext(scene_id, actor_id);

      // 如果是重命名，检查当前是否正在编辑该文件
      if (isRename) {
        if (old_path === currentActorFile.value) {
          currentActorFile.value = actor_id;
          await loadActorData(scene_id, actor_id);
          ActiveSubTab.value = 'Basic';
        }
      }
      // 打开文件操作，直接切换到单位标签并加载
      else {
        mainActiveTab.value = 'actor';
        currentActorFile.value = actor_id;
        await loadActorData(scene_id, actor_id);
        ActiveSubTab.value = 'Basic';
      }
    } else if (type === 'model') {
      // 如果是重命名，检查当前是否正在编辑该文件
      if (isRename) {
        if (old_path === currentModelFile.value) {
          currentModelFile.value = actor_id;
          await loadModelData(scene_id, actor_id);
          ActiveSubTab.value = 'Basic';
        }
      }
      // 打开文件操作，直接切换到模型标签并加载
      else {
        mainActiveTab.value = 'model';
        currentModelFile.value = actor_id;
        await loadModelData(scene_id, actor_id);
        ActiveSubTab.value = 'Basic';
      }
    } else {
      // 未知类型（multimedia、mesh、文件扩展名等），按模型处理
      if (!isRename) {
        mainActiveTab.value = 'model';
        currentModelFile.value = actor_id;
        await loadModelData(scene_id, actor_id);
        ActiveSubTab.value = 'Basic';
      }
    }
  };

  // 注册全局回调接收器
  window.onTransformUpdate = (sceneName, actorName, position, rotation, scale, type) => {
    if (
      type === 'actor' &&
      sceneName === actorData.value.parentScene &&
      actorName === actorData.value.name
    ) {
      onActorTransformUpdated(position, rotation, scale);
    } else if (
      type === 'model' &&
      sceneName === modelData.value.targetScene &&
      actorName === modelData.value.name
    ) {
      onModelTransformUpdated(position, rotation, scale);
    }
  };
};

// ========== 生命周期 ==========
onMounted(async () => {
  const result = await projectService.OnInit();
  const initData = result?.data ?? result;
  const activeScene = initData?.scenes?.[initData?.active_index ?? 0];
  await loadSceneData(activeScene?.path || DEFAULT_SCENE_NAME);

  setupWindowListener();

  coronaEventBus.on('actor-change', (type, sceneId, actorId, oldPath) => {
    if (window.onActorChange) window.onActorChange(type, sceneId, actorId, oldPath);
  });
  coronaEventBus.on('transform-update', (sceneName, actorName, position, rotation, scale, type) => {
    if (window.onTransformUpdate) window.onTransformUpdate(sceneName, actorName, position, rotation, scale, type);
  });
});

onUnmounted(() => {
  stopPlayback();
  coronaEventBus.off('actor-change');
  coronaEventBus.off('transform-update');
});
</script>
