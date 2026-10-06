#include "cef_shared_texture_probe.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <mutex>
#include <string>

#include <horizon/core/logging.h>

namespace Corona::Systems::UI {
namespace {

std::string narrow_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool try_open_texture(ID3D11Device* device, HANDLE handle) {
    ID3D11Texture2D* texture = nullptr;

    ID3D11Device1* device1 = nullptr;
    device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
    HRESULT nt_result = E_NOINTERFACE;
    if (device1 != nullptr) {
        nt_result = device1->OpenSharedResource1(
            handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        device1->Release();
    }
    if (SUCCEEDED(nt_result) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        CFW_LOG_NOTICE(
            "[CEF/OSR] 共享纹理可打开 (OpenSharedResource1): size={}x{} format={} mips={} array={}",
            desc.Width, desc.Height, static_cast<int>(desc.Format), desc.MipLevels, desc.ArraySize);
        texture->Release();
        return true;
    }

    texture = nullptr;
    const HRESULT kmt_result = device->OpenSharedResource(
        handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    if (SUCCEEDED(kmt_result) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        CFW_LOG_NOTICE(
            "[CEF/OSR] 共享纹理可打开 (OpenSharedResource/KMT): size={}x{} format={} mips={} array={}",
            desc.Width, desc.Height, static_cast<int>(desc.Format), desc.MipLevels, desc.ArraySize);
        texture->Release();
        return true;
    }

    CFW_LOG_INFO("[CEF/OSR]   该适配器打不开: OpenSharedResource1=0x{:X} OpenSharedResource=0x{:X}",
                 static_cast<unsigned>(nt_result), static_cast<unsigned>(kmt_result));
    return false;
}

}  // namespace

void probe_cef_shared_texture(std::uintptr_t raw_handle, int coded_width, int coded_height) {
    static std::mutex probe_mutex;
    static bool probed = false;

    std::lock_guard<std::mutex> lock(probe_mutex);
    if (probed) {
        return;
    }
    if (raw_handle == 0) {
        CFW_LOG_WARNING("[CEF/OSR] OnAcceleratedPaint 收到空句柄，跳过探测");
        return;
    }
    probed = true;

    CFW_LOG_NOTICE("[CEF/OSR] 共享纹理探测开始: handle=0x{:X} coded={}x{}",
                   raw_handle, coded_width, coded_height);

    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result =
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(factory_result) || factory == nullptr) {
        CFW_LOG_WARNING("[CEF/OSR] CreateDXGIFactory1 失败 (hr=0x{:X})，无法判断适配器归属",
                        static_cast<unsigned>(factory_result));
        return;
    }

    const HANDLE handle = reinterpret_cast<HANDLE>(raw_handle);
    bool opened_on_any_adapter = false;

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) != S_OK || adapter == nullptr) {
            break;
        }

        DXGI_ADAPTER_DESC adapter_desc{};
        adapter->GetDesc(&adapter_desc);
        const std::string adapter_name = narrow_utf8(adapter_desc.Description);

        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        const HRESULT created = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                                  nullptr, 0, D3D11_SDK_VERSION,
                                                  &device, &feature_level, &context);
        if (SUCCEEDED(created) && device != nullptr) {
            CFW_LOG_INFO("[CEF/OSR] adapter[{}]='{}' luid={}-{} vram={}MB",
                         index, adapter_name,
                         static_cast<long>(adapter_desc.AdapterLuid.HighPart),
                         static_cast<unsigned long>(adapter_desc.AdapterLuid.LowPart),
                         static_cast<unsigned long long>(adapter_desc.DedicatedVideoMemory) / (1024ull * 1024ull));
            opened_on_any_adapter |= try_open_texture(device, handle);
            if (context != nullptr) {
                context->Release();
            }
            device->Release();
        } else {
            CFW_LOG_INFO("[CEF/OSR] adapter[{}]='{}' 无法创建 D3D11 设备 (hr=0x{:X})",
                         index, adapter_name, static_cast<unsigned>(created));
        }
        adapter->Release();
    }

    factory->Release();

    if (opened_on_any_adapter) {
        CFW_LOG_NOTICE(
            "[CEF/OSR] 结论: 共享纹理能被 D3D11 打开；若该适配器与引擎 Vulkan 设备一致，"
            "则 D3D11_TEXTURE 导入路径可行（Step 3 仍需 Horizon 支持 D3D11 句柄类型）");
    } else {
        CFW_LOG_WARNING(
            "[CEF/OSR] 结论: 没有任何适配器能打开该共享纹理，Step 3 的 D3D11→Vulkan 导入在本机不可行");
    }
}

}  // namespace Corona::Systems::UI
