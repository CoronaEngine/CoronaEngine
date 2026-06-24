<template>
  <div class="scene-tools-panel rounded-lg overflow-hidden flex flex-col flex-1 min-h-0 h-full w-full relative bg-[#282828]/70">
    <DockTitleBar
      v-if="!isDocked"
      title="场景管理"
      extraClass="bg-[#84A65B] rounded-t-md"
      routePath="/SceneBar"
      @close="CloseFloat"
    />

    <!-- 主内容区域 -->
    <div class="flex flex-col flex-1 min-h-0">
      <div class="flex items-center gap-2 p-2 bg-[#1a1a1a]/50 border-b border-[#333]">
        <div class="text-[10px] text-gray-400 truncate flex-1">
          <span class="text-[#84a65b] font-bold">{{ currentSceneName }}</span>
          <span
            v-if="visionModeLabel"
            class="ml-2 px-1.5 py-0.5 rounded bg-[#264f78]/70 text-[#9cdcfe] border border-[#3c6f99]/60"
            :title="visionStatusTitle"
            data-testid="scene-vision-status"
          >
            {{ visionModeLabel }}
          </span>
          <span
            v-if="sceneVision.unsupported_count"
            class="ml-1 px-1.5 py-0.5 rounded bg-[#5a3a1f]/70 text-[#f0c674] border border-[#8a5a2b]/60"
            :title="visionUnsupportedTitle"
            data-testid="scene-vision-unsupported"
          >
            unsupported {{ sceneVision.unsupported_count }}
          </span>
        </div>
      </div>

      <div class="viewport-control-strip" data-testid="scenebar-viewport-controls">
        <div class="viewport-control-group" role="group" aria-label="视口 UI 模式">
          <button
            v-for="item in viewportControlState.viewportUiModes"
            :key="item.mode"
            class="viewport-mode-button"
            :class="{ active: viewportControlState.viewportUiMode === item.mode }"
            type="button"
            :title="item.title"
            @click="setViewportUiModeFromSceneBar(item.mode)"
          >
            {{ item.label }}
          </button>
        </div>
        <div class="viewport-speed-control">
          <span>速度</span>
          <input
            v-model.number="viewportControlState.cameraSpeed"
            type="range"
            min="0.01"
            max="2"
            step="0.01"
            title="摄像头移动速度"
            @input="handleCameraSpeedInput"
          />
          <strong>{{ cameraSpeedLabel }}</strong>
        </div>
      </div>

      <!-- 资源搜索栏(B-2 竞态保护 + 防抖 + 错误兜底) -->
      <div
        v-if="RESOURCE_SEARCH_ENABLED"
        class="flex items-center gap-1 px-2 py-1.5 bg-[#2a2a2a] border-b border-[#1a1a1a]"
      >
        <div class="relative flex-1">
          <input
            v-model="searchInput"
            type="text"
            placeholder="🔍 搜索资源(名称/中文/拼音,支持模糊)"
            class="w-full pl-2 pr-7 py-1 text-xs bg-[#1e1e1e] text-[#e0e0e0] border border-[#3a3a3a] rounded focus:border-[#84a65b] focus:outline-none"
            data-testid="resource-search-input"
            @input="onSearchInput"
            @keydown.enter="onSearchEnter"
            @keydown.esc="onSearchClear"
          />
          <button
            v-if="searchInput && !searchLoading"
            class="absolute right-1 top-1/2 -translate-y-1/2 text-[#666] hover:text-[#aaa] text-xs"
            data-testid="resource-search-clear"
            @click="onSearchClear"
          >
            ✕
          </button>
          <span
            v-if="searchLoading"
            class="absolute right-1 top-1/2 -translate-y-1/2 text-[#84a65b] text-[10px] animate-pulse"
          >
            ⌛
          </span>
        </div>
        <!-- 以图搜索 -->
        <label
          class="px-1.5 py-1 text-xs bg-[#3c3c3c] hover:bg-[#545454] rounded text-[#e0e0e0] cursor-pointer flex items-center"
          :class="{ 'opacity-50 pointer-events-none': searchLoading }"
          title="以图搜索(本地 pHash)"
          data-testid="resource-image-search"
        >
          🖼
          <input
            ref="imageInputRef"
            type="file"
            accept="image/*"
            class="hidden"
            @change="onImageSelected"
          />
        </label>
        <!-- 重建索引 -->
        <button
          class="px-1.5 py-1 text-xs bg-[#3c3c3c] hover:bg-[#545454] rounded text-[#e0e0e0]"
          :class="{ 'opacity-50 pointer-events-none': searchLoading }"
          title="重建索引"
          data-testid="resource-rebuild"
          @click="onRebuildIndex"
        >
          🔄
        </button>
      </div>

      <!-- 搜索结果区(可切换) -->
      <div
        v-if="RESOURCE_SEARCH_ENABLED && searchActive"
        class="flex-1 overflow-y-auto bg-[#282828]/50"
        data-testid="resource-search-results"
      >
        <!-- 错误提示 -->
        <div
          v-if="searchError"
          class="m-2 p-2 text-xs text-red-300 bg-red-900/30 border border-red-700/50 rounded"
          data-testid="resource-search-error"
        >
          ⚠ {{ searchError }}
        </div>
        <!-- 命中计数 -->
        <div v-else class="px-2 py-1 text-[10px] text-[#909090] border-b border-[#1a1a1a]/30">
          <span v-if="searchIndexing" class="text-[#84a65b]">正在准备资源索引...</span>
          <template v-else>
            找到 <span class="text-[#84a65b] font-bold">{{ searchResults.length }}</span> 项
            <span v-if="searchLastQuery" class="ml-2">query=“{{ searchLastQuery }}”</span>
            <span v-if="searchElapsedMs" class="ml-2 text-[#666]">{{ searchElapsedMs }}ms</span>
          </template>
        </div>
        <!-- 命中列表 -->
        <div
          v-for="item in searchResults"
          :key="item.path"
          class="group flex items-center px-2 py-1 hover:bg-[#3c3c3c]/50 cursor-pointer border-l-2 border-transparent hover:border-[#84a65b] text-xs"
          :class="{ 'bg-[#264f78]/60': selectedItem === 'search:' + item.path }"
          data-testid="resource-search-item"
          @click="selectedItem = 'search:' + item.path"
          @dblclick="OnLocateSearchItem(item)"
        >
          <span class="w-5 flex-shrink-0 text-center">
            <span :class="typeColorClass(item.type)">{{ typeIcon(item.type) }}</span>
          </span>
          <span class="text-[#e0e0e0] truncate flex-1 ml-1" :title="item.name">
            {{ item.name }}
          </span>
          <span class="text-[10px] text-[#666] mr-1">
            {{ item.type_label }}
          </span>
          <span
            v-if="item.score != null"
            class="text-[10px] text-[#84a65b] mr-1"
            :title="'相似度'"
          >
            {{ Math.round(item.score * 100) }}%
          </span>
          <button
            class="w-5 h-5 flex items-center justify-center text-[#666] hover:text-[#84a65b] rounded opacity-0 group-hover:opacity-100"
            title="定位到资源"
            @click.stop="OnLocateSearchItem(item)"
          >
            ◎
          </button>
        </div>
        <div
          v-if="!searchError && !searchIndexing && searchResults.length === 0"
          class="px-4 py-8 text-center text-[#666] text-xs"
        >
          暂无匹配结果
        </div>
      </div>

      <!-- 原场景树(无搜索时显示) -->
      <div v-show="!RESOURCE_SEARCH_ENABLED || !searchActive" class="flex flex-col flex-1 min-h-0">
        <div class="flex items-center gap-2 p-2 bg-[#1a1a1a]/50 border-b border-[#333]"></div>

      <!-- 工具栏 -->
      <div class="flex items-center gap-1 px-2 py-1.5 bg-[#3c3c3c]/60 border-b border-[#1a1a1a]/30">
        <!-- 导入下拉 -->
        <div class="relative">
          <button
            class="p-1.5 hover:bg-[#545454] rounded text-[#e0e0e0] text-xs flex items-center gap-1"
            title="导入"
            @click.stop="ToggleModelDropdown"
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="2"
                d="M12 4v16m8-8H4"
              ></path>
            </svg>
            <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="2"
                d="M19 9l-7 7-7-7"
              ></path>
            </svg>
          </button>
          <div
            v-if="ShowModelDropdown"
            v-click-outside="CloseModelDropdown"
            class="absolute z-20 mt-1 w-32 bg-[#3c3c3c] rounded shadow-lg border border-[#1a1a1a]"
          >
            <button
              class="block w-full px-3 py-1.5 text-xs text-[#e0e0e0] hover:bg-[#545454] text-left"
              @click.stop="HandleFileImport"
            >
              📦 模型
            </button>
            <button
              class="block w-full px-3 py-1.5 text-xs text-[#e0e0e0] hover:bg-[#545454] text-left"
              @click.stop="HandleActorImport"
            >
              👤 单位
            </button>
            <button
              class="block w-full px-3 py-1.5 text-xs text-[#e0e0e0] hover:bg-[#545454] text-left"
              @click.stop="HandleSceneImport"
            >
              🎬 场景
            </button>
            <button
              class="block w-full px-3 py-1.5 text-xs text-[#e0e0e0] hover:bg-[#545454] text-left"
              @click.stop="HandleMultimediaImport"
            >
              🎵 多媒体
            </button>
            <button
              class="block w-full px-3 py-1.5 text-xs text-[#e0e0e0] hover:bg-[#545454] text-left"
              @click.stop="HandleUiImageImport"
            >
              🖼 UI图片
            </button>
          </div>
        </div>
        <button
          class="p-1.5 hover:bg-[#545454] rounded text-[#e0e0e0]"
          title="添加灯光"
          @click.stop="ImportLightSource"
        >
          <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24">
            <path
              d="M12 2a7 7 0 0 1 7 7c0 2.38-1.19 4.47-3 5.74V17a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1v-2.26C6.19 13.47 5 11.38 5 9a7 7 0 0 1 7-7M9 21v-1h6v1a1 1 0 0 1-1 1h-4a1 1 0 0 1-1-1m3-17a5 5 0 0 0-5 5c0 2.05 1.23 3.81 3 4.58V16h4v-2.42c1.77-.77 3-2.53 3-4.58a5 5 0 0 0-5-5z"
            />
          </svg>
        </button>
        <button
          class="p-1.5 hover:bg-[#545454] rounded text-[#e0e0e0]"
          title="添加摄像头"
          @click.stop="ImportCamera"
        >
          <svg class="w-4 h-4" fill="currentColor" viewBox="0 0 24 24">
            <path
              d="M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z"
            />
          </svg>
        </button>
        <div class="w-px h-4 bg-[#1a1a1a] mx-1"></div>
        <button
          class="p-1.5 hover:bg-[#545454] rounded text-[#e0e0e0]"
          title="保存场景"
          @click.stop="SaveScene"
        >
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path
              stroke-linecap="round"
              stroke-linejoin="round"
              stroke-width="2"
              d="M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4"
            ></path>
          </svg>
        </button>
        <button
          class="p-1.5 hover:bg-[#545454] rounded text-[#e0e0e0]"
          title="截图"
          @click.stop="TakeScreenshot"
        >
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path
              stroke-linecap="round"
              stroke-linejoin="round"
              stroke-width="2"
              d="M3 9a2 2 0 012-2h.93a2 2 0 001.664-.89l.812-1.22A2 2 0 0110.07 4h3.86a2 2 0 011.664.89l.812 1.22A2 2 0 0018.07 7H19a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V9z"
            />
            <path
              stroke-linecap="round"
              stroke-linejoin="round"
              stroke-width="2"
              d="M15 13a3 3 0 11-6 0 3 3 0 016 0z"
            />
          </svg>
        </button>
        <!-- Vision / Native 渲染后端切换（仅 Vision 可用时显示） -->
        <button
          v-if="visionAvailable"
          class="p-1.5 hover:bg-[#545454] rounded flex items-center gap-0.5"
          :class="activeRenderBackend === 'vision' ? 'text-[#34d399]' : 'text-[#e0e0e0]'"
          :title="activeRenderBackend === 'vision' ? '当前: Vision (路径追踪)，点击切换到 Native' : '当前: Native (光栅化)，点击切换到 Vision'"
          @click.stop="ToggleRenderBackend"
        >
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path
              stroke-linecap="round"
              stroke-linejoin="round"
              stroke-width="2"
              d="M3 12a9 9 0 1018 0 9 9 0 00-18 0zm9-9v18m-9-9h18"
            />
          </svg>
        </button>
        <!-- GBuffer 输出模式切换 -->
        <div class="relative">
          <button
            class="p-1.5 hover:bg-[#545454] rounded flex items-center gap-0.5"
            :class="activeOutputMode === 'final_color' ? 'text-[#e0e0e0]' : 'text-[#c084fc]'"
            title="输出通道"
            @click.stop="ShowGBufferDropdown = !ShowGBufferDropdown"
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="2"
                d="M4 6h16M4 10h16M4 14h10M4 18h6"
              />
            </svg>
            <svg class="w-2.5 h-2.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="2"
                d="M19 9l-7 7-7-7"
              />
            </svg>
          </button>
          <div
            v-if="ShowGBufferDropdown"
            v-click-outside="() => (ShowGBufferDropdown = false)"
            class="absolute z-20 right-0 mt-1 w-36 bg-[#3c3c3c] rounded shadow-lg border border-[#1a1a1a]"
          >
            <div class="px-2 py-1 text-[10px] text-gray-500 border-b border-[#1a1a1a]">
              输出通道
            </div>
            <button
              v-for="buf in outputModes"
              :key="buf.type"
              class="w-full px-3 py-1.5 text-xs hover:bg-[#545454] text-left flex items-center gap-2"
              :class="activeOutputMode === buf.type ? 'text-white bg-[#4a4a4a]' : 'text-[#e0e0e0]'"
              @click.stop="SetOutputMode(buf.type)"
            >
              <span :style="{ color: buf.color }">■</span>
              {{ buf.label }}
            </button>
          </div>
        </div>
      </div>

      <!-- 输出模式快捷切换 & 截图测试 -->
      <div
        class="flex flex-wrap items-center gap-1 px-2 py-1.5 bg-[#2a2a2a] border-b border-[#1a1a1a]"
      >
        <button
          v-for="buf in outputModes"
          :key="'quick-' + buf.type"
          class="px-1.5 py-0.5 text-[10px] rounded border"
          :class="
            activeOutputMode === buf.type
              ? 'border-current font-bold'
              : 'border-[#555] text-[#999] hover:text-[#ccc] hover:border-[#777]'
          "
          :style="activeOutputMode === buf.type ? { color: buf.color, borderColor: buf.color } : {}"
          :title="'切换到 ' + buf.label"
          @click="SetOutputMode(buf.type)"
        >
          {{ buf.label }}
        </button>
        <div class="w-px h-4 bg-[#444] mx-0.5"></div>
        <button
          class="px-1.5 py-0.5 text-[10px] rounded border border-[#555] text-[#4ec9b0] hover:bg-[#3a3a3a]"
          title="快速截图（保存当前输出模式到桌面）"
          @click="QuickScreenshot"
        >
          📷 快速截图
        </button>
        <button
          class="px-1.5 py-0.5 text-[10px] rounded border border-[#555] text-[#dcdcaa] hover:bg-[#3a3a3a]"
          title="依次切换所有通道并逐一截图保存"
          @click="SaveAllBuffers"
        >
          📦 全部保存
        </button>
      </div>

      <!-- 场景树 -->
      <div class="flex-1 overflow-y-auto bg-[#282828]/50">
        <div class="select-none">
          <!-- Cameras 分组 -->
          <div
            class="flex items-center px-2 py-1 bg-[#3c3c3c]/50 border-b border-[#1a1a1a]/30 cursor-pointer"
            @click="camerasExpanded = !camerasExpanded"
          >
            <svg
              class="w-3 h-3 text-[#909090] mr-1 transition-transform"
              :class="{ 'rotate-90': camerasExpanded }"
              fill="currentColor"
              viewBox="0 0 24 24"
            >
              <path d="M10 6l6 6-6 6z" />
            </svg>
            <svg class="w-3.5 h-3.5 text-[#90caf9] mr-1" fill="currentColor" viewBox="0 0 24 24">
              <path
                d="M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z"
              />
            </svg>
            <span class="text-xs text-[#e0e0e0] font-medium">Cameras</span>
            <button
              class="ml-auto mr-2 text-sm leading-none text-[#90caf9] hover:text-white"
              title="Create camera view"
              aria-label="Create camera view"
              @click.stop="ImportCamera"
            >+</button>
            <span class="text-xs text-[#666]">{{ sceneCameras.length }}</span>
          </div>
          <div v-show="camerasExpanded" class="pl-2">
            <div v-for="cam in sceneCameras" :key="'cam-' + (cam.camera_id || cam.name)">
              <!-- Camera 行 -->
              <div
                class="group flex items-center px-2 py-0.5 hover:bg-[#3c3c3c]/50 cursor-pointer border-l-2 border-transparent hover:border-[#90caf9]"
                :class="{ 'bg-[#264f78]/60': selectedItem === 'cam:' + cam.name }"
                @mouseenter="RefreshCameraListOnHover"
                @click="SelectCamera(cam)"
                @dblclick="isCameraDeletable(cam) && OpenCameraView(cam)"
              >
                <span class="w-5 flex-shrink-0">
                  <svg class="w-4 h-4 text-[#90caf9]" fill="currentColor" viewBox="0 0 24 24">
                    <path
                      d="M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z"
                    />
                  </svg>
                </span>
                <span class="text-xs text-[#e0e0e0] truncate flex-1" :title="cam.name">
                  {{ cam.name }}
                </span>
                <span
                  class="text-[10px] text-[#666] mr-1 hidden group-hover:inline"
                  :title="'fov: ' + (cam.fov != null ? cam.fov.toFixed(1) : '-')"
                >
                  {{ cam.width }}x{{ cam.height }}
                </span>
                <button
                  v-if="isCameraDeletable(cam)"
                  class="hidden group-hover:inline text-xs leading-none text-[#888] hover:text-[#ef5350] disabled:opacity-30 disabled:hover:text-[#888]"
                  :disabled="sceneCameras.length <= 1"
                  title="Delete camera"
                  aria-label="Delete camera"
                  @click.stop="DeleteCamera(cam)"
                >x</button>
              </div>
            </div>
            <div v-if="sceneCameras.length === 0" class="px-4 py-2 text-center">
              <span class="text-[10px] text-[#666]">无相机</span>
            </div>
          </div>

          <!-- Scene Collection 分组 -->
          <div
            class="flex items-center px-2 py-1 bg-[#3c3c3c]/50 border-b border-[#1a1a1a]/30 cursor-pointer"
            @click="actorsExpanded = !actorsExpanded"
          >
            <svg
              class="w-3 h-3 text-[#909090] mr-1 transition-transform"
              :class="{ 'rotate-90': actorsExpanded }"
              fill="currentColor"
              viewBox="0 0 24 24"
            >
              <path d="M10 6l6 6-6 6z" />
            </svg>
            <span class="text-xs text-[#e0e0e0] font-medium">Scene Collection</span>
            <span class="ml-auto text-xs text-[#666]">{{ sceneImages.length }}</span>
          </div>

          <!-- 对象列表 -->
          <div v-show="actorsExpanded" class="pl-2">
            <div
              v-for="scene in sceneImages"
              :key="scene.name"
              class="group flex items-center px-2 py-0.5 hover:bg-[#3c3c3c]/50 cursor-pointer border-l-2 border-transparent hover:border-[#84a65b]"
              :class="{ 'bg-[#264f78]/60': selectedItem === scene.name }"
              @click="onActorRowClick(scene, $event)"
              @dblclick="onActorRowDoubleClick(scene, $event)"
            >
              <!-- 图标 -->
              <span class="w-5 flex-shrink-0">
                <template v-if="scene.type === 'light'">
                  <svg class="w-4 h-4 text-[#ffd54f]" fill="currentColor" viewBox="0 0 24 24">
                    <path
                      d="M12 2a7 7 0 0 1 7 7c0 2.38-1.19 4.47-3 5.74V17a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1v-2.26C6.19 13.47 5 11.38 5 9a7 7 0 0 1 7-7z"
                    />
                  </svg>
                </template>
                <template v-else-if="scene.type === 'camera'">
                  <svg class="w-4 h-4 text-[#90caf9]" fill="currentColor" viewBox="0 0 24 24">
                    <path
                      d="M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z"
                    />
                  </svg>
                </template>
                <template v-else-if="scene.type === 'video'">
                  <!-- 视频：胶片/播放图标 -->
                  <svg class="w-4 h-4 text-[#c586c0]" fill="currentColor" viewBox="0 0 24 24">
                    <path
                      d="M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zm6 4v8l6-4-6-4z"
                    />
                  </svg>
                </template>
                <template v-else-if="scene.type === 'audio'">
                  <!-- 音频：音符图标 -->
                  <svg class="w-4 h-4 text-[#dcdcaa]" fill="currentColor" viewBox="0 0 24 24">
                    <path
                      d="M12 3v10.55A4 4 0 1 0 14 17V7h4V3h-6z"
                    />
                  </svg>
                </template>
                <template v-else>
                  <svg class="w-4 h-4 text-[#e0e0e0]" fill="currentColor" viewBox="0 0 24 24">
                    <path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5" />
                  </svg>
                </template>
              </span>
              <!-- 名称 -->
              <span class="text-xs text-[#e0e0e0] truncate flex-1" :title="scene.name">
                {{ scene.name }}
              </span>
              <span
                v-if="scene.vision_proxy"
                class="text-[10px] text-[#9cdcfe] mr-1 hidden group-hover:inline"
                :title="scene.vision_binding?.shape_guid || 'Vision proxy actor'"
                data-testid="actor-vision-proxy"
              >
                Vision
              </span>
              <!-- 类型标签 -->
              <span class="text-[10px] text-[#666] mr-2 hidden group-hover:inline">
                {{ getTypeShort(scene.type) }}
              </span>
              <!-- 显隐切换按钮 -->
              <button
                class="w-5 h-5 flex items-center justify-center rounded transition-all mr-0.5"
                :class="
                  scene.visible === false
                    ? 'text-[#555] hover:text-[#999]'
                    : 'text-[#e0e0e0] hover:text-[#ffd54f]'
                "
                :title="scene.visible === false ? '显示' : '隐藏'"
                @click.stop="ToggleVisible(scene)"
                @dblclick.stop
              >
                <svg
                  v-if="scene.visible !== false"
                  class="w-3.5 h-3.5"
                  fill="none"
                  stroke="currentColor"
                  viewBox="0 0 24 24"
                >
                  <path
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    stroke-width="2"
                    d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"
                  />
                  <path
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    stroke-width="2"
                    d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z"
                  />
                </svg>
                <svg
                  v-else
                  class="w-3.5 h-3.5"
                  fill="none"
                  stroke="currentColor"
                  viewBox="0 0 24 24"
                >
                  <path
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    stroke-width="2"
                    d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.878 9.878L3 3m6.878 6.878L21 21"
                  />
                </svg>
              </button>
              <!-- 音频播放 / 停止按钮 -->
              <button
                v-if="scene.type === 'audio'"
                class="w-5 h-5 flex items-center justify-center rounded transition-all mr-0.5"
                :class="
                  (playingStates[scene.name] ?? scene._playing)
                    ? 'text-[#f48771] hover:text-[#f48771] hover:bg-red-400/20'
                    : 'text-[#dcdcaa] hover:text-[#dcdcaa] hover:bg-yellow-400/20'
                "
                :title="
                  (playingStates[scene.name] ?? scene._playing) ? '停止' : '播放'
                "
                @click.stop="handlePlayToggle(scene)"
                @dblclick.stop
              >
                <!-- 播放 ▶ -->
                <svg
                  v-if="!(playingStates[scene.name] ?? scene._playing)"
                  class="w-3.5 h-3.5"
                  fill="currentColor"
                  viewBox="0 0 24 24"
                >
                  <path d="M8 5v14l11-7z" />
                </svg>
                <!-- 停止 ■ -->
                <svg
                  v-else
                  class="w-3.5 h-3.5"
                  fill="currentColor"
                  viewBox="0 0 24 24"
                >
                  <path d="M6 6h12v12H6z" />
                </svg>
              </button>
              <!-- 删除按钮 -->
              <button
                class="w-5 h-5 flex items-center justify-center text-[#666] hover:text-red-400 hover:bg-red-400/20 rounded transition-all"
                title="删除"
                @click.stop="DeleteActor(scene)"
                @dblclick.stop
              >
                <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                  <path
                    stroke-linecap="round"
                    stroke-linejoin="round"
                    stroke-width="2"
                    d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"
                  />
                </svg>
              </button>
            </div>

            <!-- 空状态 -->
            <div v-if="sceneImages.length === 0" class="px-4 py-8 text-center">
              <span class="text-xs text-[#666]">场景为空，点击 + 添加对象</span>
            </div>
          </div>
        </div>
      </div>
      </div>

      <!-- 底部状态栏 -->
      <div
        class="flex items-center justify-between px-2 py-1 bg-[#3c3c3c]/60 border-t border-[#1a1a1a]/30 text-[10px] text-[#909090]"
      >
        <span>对象: {{ sceneImages.length }}</span>
        <span>{{ currentSceneName }}</span>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted, onUnmounted, computed } from 'vue';
import { useRoute } from 'vue-router';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { appService, sceneService, projectService, resourceService } from '@/utils/bridge.js';
import { DEFAULT_SCENE_NAME } from '@/utils/constants.js';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { setActorContext } from '@/blockly/composables/useActorContext.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import { useDockPanel } from '@/composables/useDockPanel.js';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();

const { error: logError, warn: logWarn } = useErrorHandler('SceneBar');

const EDITOR_CONTROLS_KEY = '__coronaEditorControls';
const defaultViewportControls = {
  available: false,
  viewportUiMode: 'flat2d',
  viewportUiModes: [
    { mode: 'flat2d', label: '2D UI', title: '普通屏幕 UI', active: true },
    { mode: 'stereo3d', label: '3D UI', title: '光场屏立体 UI', active: false },
  ],
  cameraSpeed: 0.2,
};
const viewportControlState = ref({ ...defaultViewportControls });
const cameraSpeedLabel = computed(() =>
  Number(viewportControlState.value.cameraSpeed || 0).toFixed(2)
);
let viewportControlPollTimer = null;
let speedApplyTimer = null;

const normalizeViewportControls = (state = {}) => {
  const modes = Array.isArray(state.viewportUiModes) && state.viewportUiModes.length > 0
    ? state.viewportUiModes
    : defaultViewportControls.viewportUiModes;
  const speed = Number(state.cameraSpeed);
  return {
    available: Boolean(state.available),
    viewportUiMode: state.viewportUiMode || defaultViewportControls.viewportUiMode,
    viewportUiModes: modes.map((item) => ({
      mode: item.mode,
      label: item.label,
      title: item.title || item.label,
      active: Boolean(item.active),
    })),
    cameraSpeed: Number.isFinite(speed) ? Math.min(2, Math.max(0.01, speed)) : defaultViewportControls.cameraSpeed,
  };
};

const getEditorControls = () => {
  if (typeof window === 'undefined') return null;
  return window[EDITOR_CONTROLS_KEY] || null;
};

const syncViewportControls = (state = {}) => {
  viewportControlState.value = normalizeViewportControls(state);
};

const requestViewportControlsState = () => {
  const controls = getEditorControls();
  if (controls && typeof controls.getState === 'function') {
    try {
      syncViewportControls(controls.getState());
      return;
    } catch (error) {
      logWarn('读取视口控制状态失败', error);
    }
  }
  appService
    .crossTabBroadcast('viewport-controls-request', { action: 'getState' })
    .catch(() => {});
};

const setViewportUiModeFromSceneBar = async (mode) => {
  const controls = getEditorControls();
  if (controls && typeof controls.setViewportUiMode === 'function') {
    const nextState = await controls.setViewportUiMode(mode);
    if (nextState !== false) syncViewportControls(nextState);
    return;
  }
  appService
    .crossTabBroadcast('viewport-controls-request', { action: 'setViewportUiMode', mode })
    .catch((error) => logWarn('切换视口 UI 模式失败', error));
};

const applyCameraSpeedFromSceneBar = async (value) => {
  const speed = Math.min(2, Math.max(0.01, Number(value) || defaultViewportControls.cameraSpeed));
  const controls = getEditorControls();
  if (controls && typeof controls.setCameraSpeed === 'function') {
    const nextState = await controls.setCameraSpeed(speed);
    if (nextState !== false) syncViewportControls(nextState);
    return;
  }
  appService
    .crossTabBroadcast('viewport-controls-request', { action: 'setCameraSpeed', value: speed })
    .catch((error) => logWarn('设置摄像头速度失败', error));
};

const handleCameraSpeedInput = () => {
  if (speedApplyTimer) {
    clearTimeout(speedApplyTimer);
  }
  speedApplyTimer = window.setTimeout(() => {
    speedApplyTimer = null;
    applyCameraSpeedFromSceneBar(viewportControlState.value.cameraSpeed);
  }, 60);
};

const onViewportControlsState = (state) => {
  syncViewportControls(state);
};

const getTypeShort = (type) => {
  const lowerType = (type || 'obj').toLowerCase();
  const typeMap = {
    light: 'Light',
    camera: 'Camera',
    obj: 'Mesh',
    fbx: 'Mesh',
    '3ds': 'Mesh',
    dae: 'Mesh',
    gltf: 'Mesh',
    glb: 'Mesh',
    stl: 'Mesh',
    mp4: 'Video',
    avi: 'Video',
    mov: 'Video',
    mp3: 'Audio',
    wav: 'Audio',
    video: 'Video',
    audio: 'Audio',
    actor: 'Actor',
    model: 'Model',
    mesh: 'Mesh',
    multimedia: 'Media',
  };
  return typeMap[lowerType] || 'Object';
};

const selectedItem = ref(null);
const selectedCameraName = ref(null);
const sceneCameras = ref([]);
const camerasExpanded = ref(true);
const actorsExpanded = ref(true);

const sceneImages = ref([]);
const sceneVision = ref({});
const playingStates = reactive({});  // { name: true/false } — 音频播放状态
const route = useRoute();
const currentSceneName = ref('');
const px = ref('1.0'),
  py = ref('1.0'),
  pz = ref('1.0');
const recording = ref(false);

const ACTOR_SINGLE_CLICK_DELAY_MS = 280;
const FOCUS_POSE_TIMEOUT_MS = 1500;
const CAMERA_FOCUS_WRITE_ATTEMPTS = 6;
const CAMERA_LIST_HOVER_REFRESH_MS = 500;
let actorSingleClickTimer = null;
let actorFocusSeq = 0;
let focusPoseRequestSeq = 0;
let previousFocusPoseResult = null;
const pendingFocusPoseRequests = new Map();
const pendingFocusCameraMoveFrames = new Set();
let lastActorFocusPose = null;
let cameraListRefreshInFlight = false;
let lastCameraListHoverRefreshAt = 0;

// ===========================================================================
//  资源智能搜索(场景栏新增功能)
// ===========================================================================
const RESOURCE_SEARCH_ENABLED = false;
const searchInput = ref('');
const searchLoading = ref(false);
const searchIndexing = ref(false);
const searchError = ref('');
const searchResults = ref([]);
const searchLastQuery = ref('');
const searchElapsedMs = ref(0);
const searchSeq = ref(0);        // B-2 竞态保护
const imageInputRef = ref(null);
const SEARCH_DEBOUNCE_MS = 600;
const SEARCH_INDEX_RETRY_MS = 250;
const SEARCH_INDEX_MAX_RETRIES = 120;
let searchDebounce = null;
let searchIndexRetry = null;

const searchActive = computed(() => {
  if (!RESOURCE_SEARCH_ENABLED) return false;
  return searchLoading.value || searchIndexing.value || searchResults.value.length > 0
    || !!searchError.value || !!searchLastQuery.value;
});

const visionModeLabel = computed(() => {
  const mode = sceneVision.value?.import_mode || '';
  if (mode === 'external_live') return 'Vision Live';
  if (mode === 'external') return 'Vision External';
  return '';
});

const visionStatusTitle = computed(() => {
  const mode = sceneVision.value?.import_mode || 'none';
  const source = sceneVision.value?.source_path || '';
  const count = sceneVision.value?.binding_count ?? 0;
  return `mode=${mode}; bindings=${count}${source ? `; source=${source}` : ''}`;
});

const visionUnsupportedTitle = computed(() => {
  const byType = sceneVision.value?.unsupported_by_type || {};
  const details = Object.entries(byType).map(([type, count]) => `${type}: ${count}`);
  return details.length ? details.join(', ') : 'Unsupported Vision shapes';
});

const typeIcon = (type) => ({
  model: '📦', actor: '👤', scene: '🎬',
  multimedia: '🎵', terrain: '🏔', script: '📜', other: '📄',
})[type] || '📄';

const typeColorClass = (type) => ({
  model: 'text-[#9cdcfe]',
  actor: 'text-[#ce9178]',
  scene: 'text-[#c586c0]',
  multimedia: 'text-[#dcdcaa]',
  terrain: 'text-[#4ec9b0]',
  script: 'text-[#b5cea8]',
  other: 'text-[#808080]',
})[type] || 'text-[#808080]';

const onSearchInput = () => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  if (searchDebounce) clearTimeout(searchDebounce);
  if (searchIndexRetry) {
    clearTimeout(searchIndexRetry);
    searchIndexRetry = null;
  }
  searchSeq.value++;
  searchLoading.value = false;
  searchIndexing.value = false;

  if (!searchInput.value.trim()) {
    searchResults.value = [];
    searchError.value = '';
    searchLastQuery.value = '';
    searchElapsedMs.value = 0;
    return;
  }

  searchDebounce = setTimeout(() => {
    searchDebounce = null;
    doFuzzySearch(searchInput.value);
  }, SEARCH_DEBOUNCE_MS);
};

const onSearchEnter = () => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  if (searchDebounce) {
    clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  if (searchIndexRetry) {
    clearTimeout(searchIndexRetry);
    searchIndexRetry = null;
  }
  doFuzzySearch(searchInput.value);
};

const onSearchClear = () => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  searchInput.value = '';
  if (searchDebounce) {
    clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  if (searchIndexRetry) {
    clearTimeout(searchIndexRetry);
    searchIndexRetry = null;
  }
  searchSeq.value++;
  searchLoading.value = false;
  searchIndexing.value = false;
  searchResults.value = [];
  searchError.value = '';
  searchLastQuery.value = '';
  searchElapsedMs.value = 0;
};

const doFuzzySearch = async (query, retryCount = 0) => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  const mySeq = ++searchSeq.value;
  if (!query || !query.trim()) {
    searchResults.value = [];
    searchError.value = '';
    searchLastQuery.value = '';
    return;
  }
  searchLoading.value = true;
  searchIndexing.value = false;
  searchError.value = '';
  try {
    const resp = await resourceService.fuzzySearch(query.trim(), 30);
    if (mySeq !== searchSeq.value) return;  // 已被新请求覆盖
    if (resp && resp.success !== false && resp.data) {
      const data = resp.data;
      if (data.status === 'success' || data.status === 'ok') {
        searchResults.value = Array.isArray(data.items) ? data.items : [];
        searchLastQuery.value = query.trim();
        searchElapsedMs.value = data.elapsed_ms || 0;
      } else if (data.status === 'indexing') {
        searchIndexing.value = true;
        searchResults.value = [];
        searchLastQuery.value = query.trim();
        searchElapsedMs.value = 0;
        if (retryCount < SEARCH_INDEX_MAX_RETRIES) {
          searchIndexRetry = setTimeout(() => {
            searchIndexRetry = null;
            if (mySeq === searchSeq.value && searchInput.value.trim() === query.trim()) {
              doFuzzySearch(query, retryCount + 1);
            }
          }, SEARCH_INDEX_RETRY_MS);
        } else {
          searchIndexing.value = false;
          searchError.value = '资源索引准备超时，请稍后重试';
        }
      } else {
        searchError.value = data.message || '搜索失败';
        searchResults.value = [];
      }
    } else {
      searchError.value = (resp && resp.error) || '搜索请求失败';
      searchResults.value = [];
    }
  } catch (e) {
    if (mySeq !== searchSeq.value) return;
    searchError.value = e?.message || '网络错误';
    searchIndexing.value = false;
    searchResults.value = [];
  } finally {
    if (mySeq === searchSeq.value) searchLoading.value = false;
  }
};

const onImageSelected = async (ev) => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  const file = ev.target.files && ev.target.files[0];
  if (!file) return;
  // B-3 大图片走 base64 时限制大小(> 2MB 警告)
  if (file.size > 5 * 1024 * 1024) {
    searchError.value = `图片过大 (${(file.size / 1024 / 1024).toFixed(1)}MB),请使用 ≤ 2MB 的图片`;
    return;
  }
  const mySeq = ++searchSeq.value;
  searchLoading.value = true;
  searchError.value = '';
  try {
    const dataUrl = await new Promise((resolve, reject) => {
      const fr = new FileReader();
      fr.onload = () => resolve(fr.result);
      fr.onerror = () => reject(fr.error);
      fr.readAsDataURL(file);
    });
    const resp = await resourceService.imageSearch(dataUrl, 30, 10);
    if (mySeq !== searchSeq.value) return;
    if (resp && resp.success !== false && resp.data) {
      const data = resp.data;
      if (data.status === 'success') {
        searchResults.value = Array.isArray(data.items) ? data.items : [];
        searchLastQuery.value = `[图] ${file.name}`;
        searchElapsedMs.value = data.elapsed_ms || 0;
      } else {
        searchError.value = data.message || '以图搜索失败';
        searchResults.value = [];
      }
    } else {
      searchError.value = (resp && resp.error) || '以图搜索请求失败';
      searchResults.value = [];
    }
  } catch (e) {
    if (mySeq !== searchSeq.value) return;
    searchError.value = e?.message || '图片读取失败';
    searchResults.value = [];
  } finally {
    if (mySeq === searchSeq.value) searchLoading.value = false;
    if (imageInputRef.value) imageInputRef.value.value = '';
  }
};

const onRebuildIndex = async () => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  const mySeq = ++searchSeq.value;
  searchLoading.value = true;
  searchError.value = '';
  try {
    const resp = await resourceService.rebuildIndex();
    if (mySeq !== searchSeq.value) return;
    if (resp && resp.success !== false && resp.data && resp.data.status === 'success') {
      searchLastQuery.value = '[重建] 已安排后台刷新';
    } else {
      searchError.value = (resp && resp.data && resp.data.message) || '重建索引失败';
    }
  } catch (e) {
    if (mySeq !== searchSeq.value) return;
    searchError.value = e?.message || '重建索引失败';
  } finally {
    if (mySeq === searchSeq.value) searchLoading.value = false;
  }
};

const OnLocateSearchItem = async (item) => {
  if (!RESOURCE_SEARCH_ENABLED) return;
  // 桥接 ResourceSearch.focus_actor → SceneTools.focus_actor
  // 资源项的 path 形如 Scene/MyScene.actor 或 assets/xxx.fbx
  // 我们尝试从 name 推断 actor,失败则回退到原行为
  try {
    const name = (item.name || '').trim();
    const resp = await resourceService.focusActor(currentSceneName.value, name);
    if (resp && resp.data && resp.data.status === 'success') {
      selectedItem.value = name;
      setActorContext(currentSceneName.value, name);
    } else {
      logWarn('定位资源失败', resp && resp.data && resp.data.message);
    }
  } catch (e) {
    logError('定位资源失败', e);
  }
};

const isMediaItem = (scene) => scene && (scene.type === 'video' || scene.type === 'audio');

const normalizeHandle = (value) => {
  const handle = Number(value);
  return Number.isFinite(handle) && handle > 0 ? handle : 0;
};

const normalizePoseVector = (value) => {
  if (!Array.isArray(value) || value.length !== 3) return null;
  const next = value.map((item) => Number(item));
  return next.every((item) => Number.isFinite(item)) ? next : null;
};

const normalizeFocusPose = (payload) => {
  const position = normalizePoseVector(payload?.position);
  const forward = normalizePoseVector(payload?.forward);
  const up = normalizePoseVector(payload?.up);
  if (!position || !forward || !up) return null;

  const center = normalizePoseVector(payload?.center);
  const distance = Number(payload?.distance);
  return {
    position,
    forward,
    up,
    center: center || null,
    distance: Number.isFinite(distance) ? distance : null,
  };
};

const clearActorSingleClickTimer = () => {
  if (actorSingleClickTimer) {
    clearTimeout(actorSingleClickTimer);
    actorSingleClickTimer = null;
  }
};

const handleFocusPoseResult = (requestId, payload) => {
  const pending = pendingFocusPoseRequests.get(requestId);
  if (!pending) return;

  clearTimeout(pending.timeout);
  pendingFocusPoseRequests.delete(requestId);

  if (!payload || payload.status === 'error') {
    pending.reject(new Error(payload?.message || 'computeActorFocusPose failed'));
    return;
  }

  const pose = normalizeFocusPose(payload);
  if (!pose) {
    pending.reject(new Error('computeActorFocusPose returned invalid pose'));
    return;
  }

  pending.resolve(pose);
};

const computeActorFocusPose = (actorHandle) => {
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.computeActorFocusPose !== 'function') {
    return Promise.reject(new Error('coronaBridge.computeActorFocusPose is unavailable'));
  }

  const requestId = `actor_focus_${Date.now()}_${++focusPoseRequestSeq}`;
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      pendingFocusPoseRequests.delete(requestId);
      reject(new Error('computeActorFocusPose timed out'));
    }, FOCUS_POSE_TIMEOUT_MS);

    pendingFocusPoseRequests.set(requestId, { resolve, reject, timeout });

    try {
      const ok = bridge.computeActorFocusPose(actorHandle, requestId);
      if (!ok) {
        clearTimeout(timeout);
        pendingFocusPoseRequests.delete(requestId);
        reject(new Error('computeActorFocusPose bridge call failed'));
      }
    } catch (e) {
      clearTimeout(timeout);
      pendingFocusPoseRequests.delete(requestId);
      reject(e);
    }
  });
};

const ControlObject = async (scene) => {
  // 音视频是独立资源，没有可操作的 Actor
  if (isMediaItem(scene)) return;
  try {
    await sceneService.openSceneActor(currentSceneName.value, scene.name);
  } catch (e) {
    logError('Failed to open actor', e);
  }
};

const SelectActor = (scene) => {
  selectedItem.value = scene.name;
  // 音视频是独立资源，仅作选中，不触发积木上下文
  if (isMediaItem(scene)) return;
  // 通知积木编辑器当前选中的物体
  setActorContext(currentSceneName.value, scene.name);
};

const SelectCamera = (cam) => {
  selectedItem.value = 'cam:' + cam.name;
  selectedCameraName.value = cam.name;
  RefreshRenderBackendState();
};

const OpenCameraView = async (cam) => {
  try {
    const cameraId = cam.camera_id || cam.id || cam.name;
    const opened = await sceneService.openCameraView(currentSceneName.value, cameraId);
    const payload = opened?.data ?? opened;
    await appService.createCameraView({
      ...(payload.camera || cam),
      scene_id: currentSceneName.value,
    });
    await OnInitObjTree();
  } catch (e) {
    logError('Failed to open camera view', e);
  }
};

const isCameraDeletable = (cam) => cam?.deletable !== false;

const DeleteCamera = async (cam) => {
  if (sceneCameras.value.length <= 1 || !isCameraDeletable(cam)) return;
  try {
    const cameraId = cam.camera_id || cam.id || cam.name;
    await appService.closeCameraView(currentSceneName.value, cameraId);
    await new Promise((resolve) => setTimeout(resolve, 0));
    await sceneService.deleteCamera(currentSceneName.value, cameraId);
    if (selectedCameraName.value === cam.name) selectedCameraName.value = null;
    if (selectedItem.value === `cam:${cam.name}`) selectedItem.value = null;
    await OnInitObjTree();
  } catch (e) {
    logError('Failed to delete camera', e);
  }
};

const isActorRowActionEvent = (event) => !!event?.target?.closest?.('button');

const resolveActorHandleForFocus = async (scene) => {
  let actorHandle = normalizeHandle(scene?.handle);
  if (actorHandle) {
    return actorHandle;
  }

  await OnInitObjTree();
  const refreshed = sceneImages.value.find((item) => item.name === scene?.name);
  actorHandle = normalizeHandle(refreshed?.handle);
  if (actorHandle && refreshed && scene) {
    scene.handle = actorHandle;
  }
  return actorHandle;
};

const getTargetCamera = () => {
  const cameraName = getTargetCameraName();
  return sceneCameras.value.find((cam) => cam.name === cameraName) || sceneCameras.value[0] || null;
};

const clearFocusPoseCache = () => {
  lastActorFocusPose = null;
};

const getCachedFocusPose = (actorHandle, cameraHandle) => {
  if (
    lastActorFocusPose &&
    lastActorFocusPose.actorHandle === actorHandle &&
    lastActorFocusPose.cameraHandle === cameraHandle
  ) {
    return lastActorFocusPose.pose;
  }
  return null;
};

const setCachedFocusPose = (actorHandle, cameraHandle, pose) => {
  lastActorFocusPose = {
    actorHandle,
    cameraHandle,
    pose,
  };
};

const queueFocusCameraMoveFrame = (callback) => {
  const rafId = window.requestAnimationFrame(() => {
    pendingFocusCameraMoveFrames.delete(rafId);
    callback();
  });
  pendingFocusCameraMoveFrames.add(rafId);
};

const sendFocusCameraMoveBurst = (bridge, cameraHandle, pose, fov, focusSeq) => {
  let attempt = 0;
  let immediateSendOk = false;

  const sendOnce = () => {
    if (focusSeq !== actorFocusSeq) {
      return;
    }

    attempt++;
    try {
      immediateSendOk =
        bridge.cameraMove(cameraHandle, pose.position, pose.forward, pose.up, fov) ||
        immediateSendOk;
    } catch (e) {
      logError('Actor focus cameraMove failed', e);
      return;
    }

    if (attempt < CAMERA_FOCUS_WRITE_ATTEMPTS) {
      queueFocusCameraMoveFrame(sendOnce);
    }
  };

  sendOnce();
  return immediateSendOk;
};

const focusActorFromList = async (scene) => {
  const focusSeq = ++actorFocusSeq;
  SelectActor(scene);

  try {
    const actorHandle = await resolveActorHandleForFocus(scene);
    if (focusSeq !== actorFocusSeq) return;
    if (!actorHandle) {
      logWarn('Actor focus skipped: missing actor handle', scene?.name);
      return;
    }

    const camera = getTargetCamera();
    const cameraHandle = normalizeHandle(camera?.handle);
    if (!cameraHandle) {
      logWarn('Actor focus skipped: missing camera handle', camera?.name);
      return;
    }

    const cachedPose = getCachedFocusPose(actorHandle, cameraHandle);
    const pose = cachedPose || await computeActorFocusPose(actorHandle);
    if (focusSeq !== actorFocusSeq) return;
    if (!cachedPose) {
      setCachedFocusPose(actorHandle, cameraHandle, pose);
    }

    const fov = Number.isFinite(Number(camera?.fov)) ? Number(camera.fov) : 45;
    const bridge = window.coronaBridge;
    if (!bridge || typeof bridge.cameraMove !== 'function') {
      logWarn('Actor focus skipped: coronaBridge.cameraMove is unavailable');
      return;
    }

    const ok = sendFocusCameraMoveBurst(bridge, cameraHandle, pose, fov, focusSeq);
    if (!ok) {
      logWarn('Actor focus skipped: cameraMove bridge call failed');
      return;
    }

    await appService.callDockFunction('', 'applyCameraPose', [
      {
        ...pose,
        fov,
        cameraHandle,
        cameraName: camera?.name,
      },
    ]);
  } catch (e) {
    if (focusSeq === actorFocusSeq) {
      logError('Actor focus failed', e);
    }
  }
};

const onActorRowClick = (scene, event) => {
  clearActorSingleClickTimer();
  SelectActor(scene);

  if (isActorRowActionEvent(event) || Number(event?.detail) > 1 || isMediaItem(scene)) {
    return;
  }

  actorSingleClickTimer = setTimeout(() => {
    actorSingleClickTimer = null;
    ControlObject(scene);
  }, ACTOR_SINGLE_CLICK_DELAY_MS);
};

const onActorRowDoubleClick = (scene, event) => {
  clearActorSingleClickTimer();
  SelectActor(scene);

  if (isActorRowActionEvent(event)) {
    return;
  }

  if (scene?.type === 'audio') {
    handlePlayToggle(scene);
    return;
  }

  if (scene?.type === 'video') {
    return;
  }

  focusActorFromList(scene);
};

/// 切换音频播放/停止
const handlePlayToggle = async (scene) => {
  const rid = scene.resourceId;
  if (!rid) {
    logWarn('[audio] No resource_id for', scene.name);
    return;
  }
  const key = scene.name;
  const playing = playingStates[key] ?? scene._playing ?? false;
  if (playing) {
    // 停止
    try {
      await sceneService.stopAudio(rid);
    } catch (e) {
      logError('[audio] stop failed', e);
    }
    playingStates[key] = false;
    if (scene._playing !== undefined) scene._playing = false;
  } else {
    // 播放（单次，不循环）
    try {
      await sceneService.playAudio(rid, false);
    } catch (e) {
      logError('[audio] play failed', e);
    }
    playingStates[key] = true;
    if (scene._playing !== undefined) scene._playing = true;
  }
};

const ToggleVisible = async (scene) => {
  const newVisible = scene.visible === false ? true : false;
  scene.visible = newVisible;
  // 音视频资源没有对应 Actor，仅在前端切换可见标记
  if (isMediaItem(scene)) return;
  try {
    await sceneService.actorOperation(currentSceneName.value, scene.name, 'SetVisible', [
      newVisible ? 1 : 0,
    ]);
  } catch (e) {
    scene.visible = !newVisible;
    logError('Failed to toggle visibility', e);
  }
};

const SaveScene = async () => {
  try {
    await projectService.sceneSave(currentSceneName.value);
  } catch (e) {
    logError('Failed to save scene', e);
  }
};

const getTargetCameraName = () => {
  if (
    selectedCameraName.value &&
    sceneCameras.value.some((cam) => cam.name === selectedCameraName.value)
  ) {
    return selectedCameraName.value;
  }

  const item = selectedItem.value || '';
  if (item.startsWith('cam:')) {
    const cameraName = item.slice(4);
    if (sceneCameras.value.some((cam) => cam.name === cameraName)) {
      return cameraName;
    }
  }
  return sceneCameras.value[0]?.name;
};

const TakeScreenshot = async () => {
  try {
    const cameraName = getTargetCameraName();
    const selectResult = await sceneService.selectScreenshotPath(
      currentSceneName.value,
      cameraName
    );
    const selectPayload = selectResult?.data ?? selectResult;

    if (!selectPayload || selectPayload.status === 'canceled' || !selectPayload.path) {
      return;
    }

    const result = await sceneService.saveScreenshot(
      currentSceneName.value,
      selectPayload.path,
      cameraName
    );
    const payload = result?.data ?? result;
    if (result?.success === false || payload?.status === 'error') {
      logError('Screenshot failed', payload?.message || result?.error || 'unknown error');
    }
  } catch (e) {
    logError('Failed to take screenshot', e);
  }
};

const outputModes = [
  { type: 'final_color', label: 'Final Color', color: '#e0e0e0' },
  { type: 'base_color', label: 'Base Color', color: '#60a5fa' },
  { type: 'normal', label: 'Normal', color: '#34d399' },
  { type: 'position', label: 'Position', color: '#fbbf24' },
  { type: 'object_id', label: 'Object ID', color: '#c084fc' },
];
const ShowGBufferDropdown = ref(false);
const activeOutputMode = ref('final_color');

// Vision / Native 渲染后端切换状态
const visionAvailable = ref(false);
const activeRenderBackend = ref('native');

const RefreshRenderBackendState = async () => {
  try {
    const availResult = await sceneService.isVisionAvailable();
    const availPayload = availResult?.data ?? availResult;
    visionAvailable.value = !!availPayload?.available;
    if (!visionAvailable.value) {
      return;
    }
    const target = getTargetCamera();
    const modeResult = await sceneService.getRenderBackend(
      currentSceneName.value,
      target?.camera_id || target?.name || null,
    );
    const modePayload = modeResult?.data ?? modeResult;
    if (modePayload?.mode) {
      activeRenderBackend.value = modePayload.mode;
    }
  } catch (e) {
    logError('Failed to query render backend state', e);
  }
};

const ToggleRenderBackend = async () => {
  const next = activeRenderBackend.value === 'vision' ? 'native' : 'vision';
  try {
    const target = getTargetCamera();
    const result = await sceneService.setRenderBackend(
      next,
      currentSceneName.value,
      target?.camera_id || target?.name || null,
    );
    const payload = result?.data ?? result;
    if (result?.success === false || payload?.status === 'error') {
      logError('Switch render backend failed', payload?.message || result?.error || 'unknown error');
    } else {
      activeRenderBackend.value = next;
    }
  } catch (e) {
    logError('Failed to switch render backend', e);
  }
};

const SetOutputMode = async (mode) => {
  ShowGBufferDropdown.value = false;
  try {
    const cameraName = getTargetCameraName();
    const result = await sceneService.setOutputMode(currentSceneName.value, cameraName, mode);
    const payload = result?.data ?? result;
    if (result?.success === false || payload?.status === 'error') {
      logError(`Set output mode failed`, payload?.message || result?.error || 'unknown error');
    } else {
      activeOutputMode.value = mode;
    }
  } catch (e) {
    logError('Failed to set output mode', e);
  }
};

const QuickScreenshot = async () => {
  try {
    const cameraName = getTargetCameraName();
    const selectResult = await sceneService.selectScreenshotPath(
      currentSceneName.value,
      cameraName
    );
    const selectPayload = selectResult?.data ?? selectResult;
    if (!selectPayload || selectPayload.status === 'canceled' || !selectPayload.path) {
      return;
    }
    // Insert current mode name before file extension
    const path = selectPayload.path;
    const dotIdx = path.lastIndexOf('.');
    const taggedPath =
      dotIdx > 0
        ? path.slice(0, dotIdx) + '_' + activeOutputMode.value + path.slice(dotIdx)
        : path + '_' + activeOutputMode.value + '.png';

    const result = await sceneService.saveScreenshot(
      currentSceneName.value,
      taggedPath,
      cameraName
    );
    const payload = result?.data ?? result;
    if (result?.success === false || payload?.status === 'error') {
      logError('Quick screenshot failed', payload?.message || result?.error || 'unknown error');
    }
  } catch (e) {
    logError('Failed to take quick screenshot', e);
  }
};

const SaveAllBuffers = async () => {
  try {
    const cameraName = getTargetCameraName();
    const selectResult = await sceneService.selectScreenshotPath(
      currentSceneName.value,
      cameraName
    );
    const selectPayload = selectResult?.data ?? selectResult;
    if (!selectPayload || selectPayload.status === 'canceled' || !selectPayload.path) {
      return;
    }
    const basePath = selectPayload.path;
    const dotIdx = basePath.lastIndexOf('.');
    const stem = dotIdx > 0 ? basePath.slice(0, dotIdx) : basePath;
    const ext = dotIdx > 0 ? basePath.slice(dotIdx) : '.png';

    const previousMode = activeOutputMode.value;

    for (const buf of outputModes) {
      // Switch mode
      await sceneService.setOutputMode(currentSceneName.value, cameraName, buf.type);
      activeOutputMode.value = buf.type;

      // Wait one frame for GPU to render with new mode
      await new Promise((resolve) => setTimeout(resolve, 200));

      // Save screenshot
      const filePath = stem + '_' + buf.type + ext;
      const result = await sceneService.saveScreenshot(
        currentSceneName.value,
        filePath,
        cameraName
      );
      const payload = result?.data ?? result;
      if (result?.success === false || payload?.status === 'error') {
        logError(`Save ${buf.label} failed`, payload?.message || result?.error || 'unknown error');
      }
    }

    // Restore previous mode
    await sceneService.setOutputMode(currentSceneName.value, cameraName, previousMode);
    activeOutputMode.value = previousMode;
  } catch (e) {
    logError('Failed to save all buffers', e);
  }
};

const ShowModelDropdown = ref(false);
const ToggleModelDropdown = () => {
  ShowModelDropdown.value = !ShowModelDropdown.value;
};
const CloseModelDropdown = () => {
  ShowModelDropdown.value = false;
};

const generateUniqueName = (baseName) => {
  let name = baseName;
  let counter = 1;
  while (sceneImages.value.find((item) => item.name === name)) {
    name = `${baseName}_${counter}`;
    counter++;
  }
  return name;
};

const LIGHT_MODEL_PATH = 'assets/editor/Ball.obj';

const ImportLightSource = async () => {
  ShowModelDropdown.value = false;
  const lightName = generateUniqueName('Light');
  await addActorToList({
    name: lightName,
    path: LIGHT_MODEL_PATH,
    type: 'light',
  });
};

const ImportCamera = async () => {
  ShowModelDropdown.value = false;
  try {
    const existingNames = new Set(sceneCameras.value.map((camera) => camera.name));
    let cameraName = 'Camera';
    let suffix = 1;
    while (existingNames.has(cameraName)) {
      cameraName = `Camera_${suffix++}`;
    }
    const result = await sceneService.createCameraView(currentSceneName.value, cameraName);
    const payload = result?.data ?? result;
    if (!payload?.camera) throw new Error(payload?.message || 'Camera creation failed');
    await appService.createCameraView({
      ...payload.camera,
      scene_id: currentSceneName.value,
    });
    selectedCameraName.value = payload.camera.name;
    await OnInitObjTree();
  } catch (e) {
    logError('Failed to create camera view', e);
  }
};

const addActorToList = async (actor) => {
  if (!actor || !actor.name) return;
  sceneImages.value.push({
    name: actor.name,
    path: actor.path,
    type: actor.type || 'obj',
    visible: actor.visible !== false,
    handle: normalizeHandle(actor.handle),
  });
};

const HandleFileImport = async () => {
  // 测试 window.cefQuery 是否存在
  if (typeof window.cefQuery === 'undefined') {
    alert('错误：window.cefQuery 未定义！CEF bridge 未初始化。');
    return;
  }

  ShowModelDropdown.value = false;
  if (!currentSceneName.value) {
    logWarn('File import aborted: no active scene');
    return;
  }
  try {
    await appService.callDockFunction('', 'showLoading', ['加载中', '请稍候...', 0]);
  } catch (e) {
    // showLoading 失败不阻塞主流程
  }
  try {
    const result = await projectService.importResourceFileByDialog(currentSceneName.value, 'model');
    // 兼容两种返回形态:
    //   1) 包装型 { success, data: { status, actor, ... } }
    //   2) 直返型 { status, actor, ... }
    const payload = result?.data ?? result;
    const status = payload?.status;
    if (result?.success === false || status === 'error') {
      logError('File import failed', payload?.message || result?.error || 'unknown error');
      return;
    }
    if (status === 'canceled') {
      // 用户主动取消,无需弹错
      return;
    }
    const actor = payload?.actor;
    if (actor && actor.name) {
      await addActorToList(actor);
      try {
        await appService.callDockFunction('', 'updateLoading', ['导入完成', 100]);
      } catch (e) {
        // 忽略加载条更新失败
      }
    } else {
      logWarn('File import returned without actor payload', payload);
    }
  } catch (e) {
    logError('File import failed', e);
  } finally {
    try {
      await appService.callDockFunction('', 'hideLoading', []);
    } catch (e) {
      // 忽略关闭加载条失败
    }
  }
};

const HandleUiImageImport = async () => {
  // 导入一张图片，自动创建一个带该图为纹理的 quad（光场 UI 平面），默认作为 UI。
  if (typeof window.cefQuery === 'undefined') {
    alert('错误：window.cefQuery 未定义！CEF bridge 未初始化。');
    return;
  }

  ShowModelDropdown.value = false;
  if (!currentSceneName.value) {
    logWarn('UI image import aborted: no active scene');
    return;
  }
  try {
    await appService.callDockFunction('', 'showLoading', ['加载中', '请稍候...', 0]);
  } catch (e) {
    // showLoading 失败不阻塞主流程
  }
  try {
    const result = await projectService.importResourceFileByDialog(currentSceneName.value, 'ui_image');
    const payload = result?.data ?? result;
    const status = payload?.status;
    if (result?.success === false || status === 'error') {
      logError('UI image import failed', payload?.message || result?.error || 'unknown error');
      return;
    }
    if (status === 'canceled') {
      return;
    }
    const actor = payload?.actor;
    if (actor && actor.name) {
      await addActorToList(actor);
      try {
        await appService.callDockFunction('', 'updateLoading', ['导入完成', 100]);
      } catch (e) {
        // 忽略加载条更新失败
      }
    } else {
      logWarn('UI image import returned without actor payload', payload);
    }
  } catch (e) {
    logError('UI image import failed', e);
  } finally {
    try {
      await appService.callDockFunction('', 'hideLoading', []);
    } catch (e) {
      // 忽略关闭加载条失败
    }
  }
};

const HandleActorImport = async () => {
  ShowModelDropdown.value = false;
  await appService.callDockFunction('', 'showLoading', ['加载中', '请稍候...', 0]);
  try {
    const result = await projectService.importResourceFileByDialog(currentSceneName.value, 'actor');
    if (result.success && result.data.actor) {
      await addActorToList(result.data.actor);
      await appService.callDockFunction('', 'updateLoading', ['导入完成', 100]);
    }
  } catch (e) {
    logError('Actor import failed', e);
  }
  await appService.callDockFunction('', 'hideLoading', []);
};

const HandleMultimediaImport = async () => {
  ShowModelDropdown.value = false;
  await appService.callDockFunction('', 'showLoading', ['加载中', '请稍候...', 0]);
  try {
    const result = await projectService.importResourceFileByDialog(
      currentSceneName.value,
      'multimedia'
    );
    // 兼容包装型 { success, data } 与直返型两种形态
    const payload = result?.data ?? result;
    const status = payload?.status;
    if (result?.success === false || status === 'error') {
      logError('Multimedia import failed', payload?.message || result?.error || 'unknown error');
      return;
    }
    if (status === 'canceled') {
      return;
    }
    // 音视频是独立资源（非 Actor），加入资源列表
    const media = payload?.media;
    if (media && media.name) {
      await addMediaToList(media);
      await appService.callDockFunction('', 'updateLoading', ['导入完成', 100]);
    }
  } catch (e) {
    logError('Multimedia import failed', e);
  }
  await appService.callDockFunction('', 'hideLoading', []);
};

const addMediaToList = async (media) => {
  if (!media || !media.name) return;
  // media.type 为 'video' / 'audio'
  sceneImages.value.push({
    name: media.name,
    path: media.path,
    type: media.type || 'multimedia',
    visible: true,
    resourceId: media.resource_id,
    duration: media.duration,
    codec: media.codec,
    width: media.width,
    height: media.height,
    fps: media.fps,
    sampleRate: media.sample_rate,
    channels: media.channels,
  });
};

const HandleSceneImport = async () => {
  ShowModelDropdown.value = false;
  await appService.callDockFunction('', 'showLoading', ['加载中', '请稍候...', 0]);
  try {
    const result = await projectService.importResourceFileByDialog(currentSceneName.value, 'scene');
    const payload = result?.data ?? result;
    const status = payload?.status;
    if (result?.success === false || status === 'error') {
      logError('Scene import failed', payload?.message || result?.error || 'unknown error');
      return;
    }
    if (status === 'canceled') {
      return;
    }

    await appService.callDockFunction('', 'updateLoading', ['导入中', 40]);
    await OnInitObjTree();
    await appService.callDockFunction('', 'updateLoading', ['导入完成', 100]);
  } catch (e) {
    logError('Scene import failed', e);
  } finally {
    await appService.callDockFunction('', 'hideLoading', []);
  }
};

const DeleteActor = async (scene) => {
  clearFocusPoseCache();
  sceneImages.value = sceneImages.value.filter((item) => item.name !== scene.name);

  try {
    await sceneService.removeActor(currentSceneName.value, scene.name);
  } catch {
    // 忽略
  }
};

const CloseFloat = async () => {
  if (closeDockPanel) { closeDockPanel(); return; }
};

const setupFragmentListener = () => {
  window.onFragmentChanged = (fragment) => {
    const queryString = fragment.split('?')[1];
    if (!queryString) return;
    console.log(queryString);
    const params = new URLSearchParams(queryString);
    const sceneName = params.get('sceneName');

    if (sceneName) {
      // 无论 sceneName 是否变化，都要刷新 actor 列表
      // 场景切换后后端数据已更新，必须重新拉取
      currentSceneName.value = sceneName;
      OnInitObjTree();
    }
  };

  window.onSceneTreeChanged = (sceneName) => {
    if (!sceneName || sceneName === currentSceneName.value) {
      OnInitObjTree();
    }
  };
};

const OnInitObjTree = async () => {
  try {
    clearFocusPoseCache();
    const result = await sceneService.listSceneTree(currentSceneName.value);
    sceneImages.value = [];
    sceneCameras.value = [];
    sceneVision.value = {};

    if (result.success && result.data) {
      const data = result.data;
      sceneVision.value = data.vision || {};

      if (Array.isArray(data.actors)) {
        data.actors.forEach((item) => {
          sceneImages.value.push({
            name: item.name,
            path: item.path,
            type: item.type || 'obj',
            visible: item.visible !== false,
            handle: normalizeHandle(item.handle),
            vision_proxy: item.vision_proxy === true,
            vision_binding: item.vision_binding || null,
          });
        });
      }

      if (Array.isArray(data.cameras)) {
        applyCameraList(data.cameras);
      }
    }
  } catch (e) {
    logError('Failed to load scene tree', e);
  }
};

onMounted(async () => {
  previousFocusPoseResult = window.__coronaFocusPoseResult;
  window.__coronaFocusPoseResult = handleFocusPoseResult;

  const result = await projectService.OnInit();
  if (RESOURCE_SEARCH_ENABLED) {
    resourceService.prepareIndex().catch((error) => {
      logWarn('资源索引预热失败', error);
    });
  }
  const queryString = window.location.hash?.split('?')[1] || window.location.search?.slice(1);
  const urlSceneName = queryString ? new URLSearchParams(queryString).get('sceneName') : null;

  // 从 OnInit 返回值中取活跃场景：scenes 数组 + active_index
  const initData = result?.data ?? result;
  const activeScene = initData?.scenes?.[initData?.active_index ?? 0];
  currentSceneName.value = urlSceneName || activeScene?.path || DEFAULT_SCENE_NAME;

  setupFragmentListener();
  await OnInitObjTree();
  await RefreshRenderBackendState();
  requestViewportControlsState();
  viewportControlPollTimer = window.setInterval(requestViewportControlsState, 1000);

  // 监听 Python 推送的 actor-change：场景切换/物体变化时重新加载场景树
  coronaEventBus.on('viewport-controls-state', onViewportControlsState);
  coronaEventBus.on('actor-change', onActorChangeEvent);
  coronaEventBus.on('scene-tree-changed', onSceneTreeChangedEvent);
});

// 场景切换或 actor 变化时刷新当前场景树
const onActorChangeEvent = (type, sceneId /*, actorId, oldPath */) => {
  if (type === 'scene' && sceneId) {
    currentSceneName.value = sceneId;
  }
  OnInitObjTree();
};

const onSceneTreeChangedEvent = (sceneName) => {
  if (!sceneName || sceneName === currentSceneName.value) {
    OnInitObjTree();
  }
};

const normalizeCameraPayload = (cam) => ({
  id: cam.id || cam.camera_id || cam.name,
  camera_id: cam.camera_id || cam.id || cam.name,
  name: cam.name || 'Camera',
  width: cam.width || 0,
  height: cam.height || 0,
  fov: cam.fov ?? null,
  handle: normalizeHandle(cam.handle ?? cam.camera_handle),
  render_backend: cam.render_backend || 'native',
  output_mode: cam.output_mode || 'final_color',
  deletable: cam.deletable !== false,
  move_speed: cam.move_speed || 1,
  view_open: !!cam.view_open,
  view_x: cam.view_x || 120,
  view_y: cam.view_y || 120,
  view_width: cam.view_width || 960,
  view_height: cam.view_height || 540,
});

const applyCameraList = (cameras) => {
  sceneCameras.value = cameras.map(normalizeCameraPayload);
  if (!sceneCameras.value.some((cam) => cam.name === selectedCameraName.value)) {
    selectedCameraName.value = sceneCameras.value[0]?.name || null;
  }
};

const RefreshCameraListOnly = async () => {
  if (!currentSceneName.value || cameraListRefreshInFlight) {
    return;
  }
  cameraListRefreshInFlight = true;
  try {
    const result = await sceneService.listSceneTree(currentSceneName.value);
    const data = result?.data ?? result;
    if (Array.isArray(data?.cameras)) {
      applyCameraList(data.cameras);
    }
  } catch (e) {
    logError('Failed to refresh camera list', e);
  } finally {
    cameraListRefreshInFlight = false;
  }
};

const RefreshCameraListOnHover = () => {
  const now = Date.now();
  if (now - lastCameraListHoverRefreshAt < CAMERA_LIST_HOVER_REFRESH_MS) {
    return;
  }
  lastCameraListHoverRefreshAt = now;
  RefreshCameraListOnly();
};

onUnmounted(() => {
  if (searchDebounce) {
    clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  if (searchIndexRetry) {
    clearTimeout(searchIndexRetry);
    searchIndexRetry = null;
  }
  if (viewportControlPollTimer) {
    clearInterval(viewportControlPollTimer);
    viewportControlPollTimer = null;
  }
  if (speedApplyTimer) {
    clearTimeout(speedApplyTimer);
    speedApplyTimer = null;
  }
  clearFocusPoseCache();
  clearActorSingleClickTimer();
  actorFocusSeq++;
  pendingFocusPoseRequests.forEach((pending) => {
    clearTimeout(pending.timeout);
    pending.reject(new Error('SceneBar unmounted'));
  });
  pendingFocusPoseRequests.clear();
  pendingFocusCameraMoveFrames.forEach((rafId) => window.cancelAnimationFrame(rafId));
  pendingFocusCameraMoveFrames.clear();
  if (window.__coronaFocusPoseResult === handleFocusPoseResult) {
    window.__coronaFocusPoseResult = previousFocusPoseResult;
  }

  coronaEventBus.off('viewport-controls-state', onViewportControlsState);
  coronaEventBus.off('actor-change', onActorChangeEvent);
  coronaEventBus.off('scene-tree-changed', onSceneTreeChangedEvent);
});
</script>

<style scoped>
.scene-tools-panel {
  background: linear-gradient(180deg, rgba(38, 42, 38, 0.96), rgba(28, 31, 29, 0.98));
  border: 1px solid rgba(255, 255, 255, 0.08);
  box-shadow: 0 18px 42px rgba(0, 0, 0, 0.34);
}

.viewport-control-strip {
  display: grid;
  grid-template-columns: minmax(124px, auto) minmax(0, 1fr);
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border-bottom: 1px solid rgba(0, 0, 0, 0.34);
  background:
    linear-gradient(180deg, rgba(31, 35, 31, 0.96), rgba(24, 27, 24, 0.96)),
    #1b1f1b;
}

.viewport-control-group {
  display: inline-grid;
  grid-template-columns: repeat(2, minmax(52px, 1fr));
  gap: 4px;
  padding: 3px;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 6px;
  background: rgba(11, 13, 12, 0.58);
}

.viewport-mode-button {
  height: 25px;
  padding: 0 8px;
  border: 1px solid transparent;
  border-radius: 4px;
  color: #b9c5b2;
  background: transparent;
  font-size: 11px;
  font-weight: 600;
  white-space: nowrap;
  transition:
    background-color 140ms ease,
    border-color 140ms ease,
    color 140ms ease,
    transform 140ms ease;
}

.viewport-mode-button:hover {
  color: #ffffff;
  background: rgba(255, 255, 255, 0.08);
}

.viewport-mode-button.active {
  color: #ffffff;
  border-color: rgba(138, 166, 106, 0.5);
  background: rgba(138, 166, 106, 0.24);
}

.viewport-mode-button:active {
  transform: translateY(1px);
}

.viewport-mode-button:focus-visible {
  outline: 2px solid rgba(138, 166, 106, 0.72);
  outline-offset: 1px;
}

.viewport-speed-control {
  min-width: 0;
  display: grid;
  grid-template-columns: auto minmax(72px, 1fr) 34px;
  align-items: center;
  gap: 8px;
  color: #aeb9aa;
  font-size: 11px;
}

.viewport-speed-control input[type='range'] {
  width: 100%;
  height: 3px;
  accent-color: #8aa66a;
  cursor: pointer;
}

.viewport-speed-control strong {
  color: #dfe8da;
  font-variant-numeric: tabular-nums;
  font-size: 11px;
  font-weight: 600;
  text-align: right;
}

@media (max-width: 380px) {
  .viewport-control-strip {
    grid-template-columns: 1fr;
  }
}
</style>
