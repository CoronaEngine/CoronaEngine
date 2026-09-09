/**
 * 小世界持久化边界：只调用 editorApi.ugc，不拼接绝对路径、不使用 localStorage。
 * 引擎侧负责将 worldId 安全地映射到当前活动项目的 ugc/worlds 目录。
 */
import { editorApi } from '../../api/editorApi.js';
import {
  UGC_WORLD_VERSION,
  deserializeUgcWorld,
  isValidUgcWorldId,
  serializeUgcWorld,
} from './ugcWorldState.js';

/**
 * 解包 Editor API 响应，并拒绝 null、undefined 和非对象结果。
 *
 * @param {unknown} response Editor API 原始响应。
 * @returns {object} API data 对象。
 */
function unwrapResponse(response) {
  if (response === null || response === undefined) {
    throw new Error('小世界存档 API 返回空响应。');
  }

  if (
    response.success === false ||
    response.ok === false ||
    response.status === 'error' ||
    response.type === 'error' ||
    response.error
  ) {
    const error = new Error(response.error || response.message || '小世界存档操作失败。');
    error.code = response.error_code ?? response.code;
    throw error;
  }

  const data = response.data ?? response;
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    throw new Error('小世界存档 API 返回数据无效。');
  }

  return data;
}

/**
 * 校验保存回执，防止错误项目或错误版本被当成保存成功。
 *
 * @param {object} receipt 保存回执。
 * @param {string} worldId 请求的世界 ID。
 * @returns {object} 原始保存回执。
 */
function validateSaveReceipt(receipt, worldId) {
  const expectedRelativePath = `ugc/worlds/${worldId}.json`;

  if (receipt.worldId !== worldId) {
    throw new Error('保存回执中的 worldId 与请求不一致。');
  }
  if (receipt.version !== UGC_WORLD_VERSION) {
    throw new Error('保存回执中的小世界版本不受支持。');
  }
  if (receipt.relativePath !== expectedRelativePath) {
    throw new Error('保存回执中的文件路径与当前小世界不一致。');
  }

  return receipt;
}

/** 校验 worldId；正式路径由引擎侧根据当前项目上下文拼接。 */
function ensureWorldId(worldId) {
  if (!isValidUgcWorldId(worldId)) {
    throw new Error('小世界 worldId 无效。');
  }
}

function normalizeError(error, fallback) {
  if (error instanceof Error) return error;
  return Object.assign(new Error(error?.message || error?.error || fallback), {
    code: error?.code ?? error?.error_code,
  });
}

function ensureApi(api, method) {
  const handler = api?.ugc?.[method];
  if (typeof handler !== 'function') {
    throw new Error(`UGC API 不可用：ugc.${method}`);
  }
  return handler;
}

/**
 * 创建小世界持久化服务。
 *
 * @param {object} options 可注入 api，便于单元测试和未来底层适配。
 * @returns {object} 小世界存档 API。
 */
export function createUgcWorldPersistence({ api = editorApi } = {}) {
  return {
    async listWorlds() {
      try {
        const response = await ensureApi(api, 'listWorlds')();
        const data = unwrapResponse(response);
        return Array.isArray(data.worlds) ? data.worlds : [];
      } catch (error) {
        throw normalizeError(error, '读取小世界列表失败。');
      }
    },

    async loadWorld(worldId) {
      ensureWorldId(worldId);
      try {
        const response = await ensureApi(api, 'loadWorld')({ worldId });
        const data = unwrapResponse(response);
        const world = deserializeUgcWorld(data.worldData ?? data);
        if (world.id !== worldId) {
          throw new Error('存档 ID 与请求不一致。');
        }
        return world;
      } catch (error) {
        throw normalizeError(error, `加载小世界失败：${worldId}。`);
      }
    },

    async saveWorld(worldId, worldData) {
      ensureWorldId(worldId);
      let payload;
      try {
        payload = serializeUgcWorld(worldData);
        if (payload.world.id !== worldId) {
          throw new Error('存档 ID 与请求不一致。');
        }
      } catch (error) {
        throw normalizeError(error, '小世界数据校验失败。');
      }

      try {
        const response = await ensureApi(
          api,
          'saveWorld'
        )({
          worldId,
          worldData: payload,
        });
        return validateSaveReceipt(unwrapResponse(response), worldId);
      } catch (error) {
        throw normalizeError(error, `保存小世界失败：${worldId}。`);
      }
    },

    async deleteWorld(worldId) {
      ensureWorldId(worldId);
      try {
        const response = await ensureApi(api, 'deleteWorld')({ worldId });
        return unwrapResponse(response);
      } catch (error) {
        throw normalizeError(error, `删除小世界失败：${worldId}。`);
      }
    },
  };
}
