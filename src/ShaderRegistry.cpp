// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Globals.h"
#include "Hooks.h"
#include "ShaderReflection.h"
#include "ShaderRegistry.h"
#include "Tessellation.h"

#include <shared_mutex>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <dxgi.h>
#include <d3d11_4.h>

namespace ShaderRegistry
{
	namespace
	{
		// ID3D11Device vtable: CreateVertexShader. ID3D11DeviceContext vtable: DrawIndexed,
		// DrawIndexedInstanced.
		constexpr size_t kCreateVertexShaderSlot = 12;
		constexpr size_t kCreateDeferredContextSlot = 27;
		constexpr size_t kDrawIndexedSlot = 12;
		constexpr size_t kDrawIndexedInstancedSlot = 20;

		using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(
			ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

		CreateVertexShaderFn   g_original{ nullptr };
		CreateDeferredContextFn g_createDeferredContext{ nullptr };
		std::mutex g_installLock;
		std::atomic<ID3D11Device*> g_rendererDevice{ nullptr };

		struct ContextHooks
		{
			void** nativeTable{ nullptr };
			std::unique_ptr<void*[]> table;
			void* forwarders{ nullptr }; // Process lifetime, like the hooked COM table.
			size_t slots{ 0 };
		};
		std::shared_mutex g_contextLock;
		// Immediate and deferred contexts have different vtables and original functions.
		// Nodes are never erased, so the per-thread cache remains valid across rehashes.
		std::unordered_map<void**, ContextHooks> g_contextHooks;
		// Private data follows the COM object's lifetime, unlike an address-keyed map:
		// a destroyed deferred context's address can be reused by a fresh context.
		// This stores bytes, not an interface, so it holds no context/device reference.
		constexpr GUID kContextHooksCookie{ 0x24b82438, 0x6438, 0x4673, { 0x9b, 0xb6, 0x69, 0x2d, 0xb4, 0x7d, 0x3b, 0x3f } };

		const ContextHooks* ContextCookie(ID3D11DeviceContext* a_context)
		{
			const ContextHooks* hooks = nullptr;
			UINT size = sizeof(hooks);
			return SUCCEEDED(a_context->GetPrivateData(kContextHooksCookie, &size, &hooks)) && size == sizeof(hooks) ?
			           hooks : nullptr;
		}

		const ContextHooks& HooksFor(ID3D11DeviceContext* a_context)
		{
			auto** table = *reinterpret_cast<void***>(a_context);
			thread_local void** lastTable = nullptr;
			thread_local ID3D11DeviceContext* lastContext = nullptr;
			thread_local const ContextHooks* lastHooks = nullptr;
			if (table != lastTable || a_context != lastContext) {
				const std::shared_lock lock(g_contextLock);
				const auto found = g_contextHooks.find(table);
				// Another renderer can clone our table and chain its draw hook to this
				// detour. Its vptr is then different, but the COM context is still ours.
				lastHooks = found != g_contextHooks.end() ? &found->second : ContextCookie(a_context);
				if (!lastHooks) {
					throw std::runtime_error("ShaderRegistry: chained context lost its hook cookie");
				}
				lastTable = table;
				lastContext = a_context;
			}
			return *lastHooks;
		}

		std::shared_mutex g_lock;
		size_t            g_bytes{ 0 };

		std::unordered_map<ID3D11VertexShader*, std::vector<uint8_t>> g_signatures;

		bool g_installed{ false };

		bool PatchSlot(void** a_table, size_t a_slot, void* a_detour, void** a_original)
		{
			DWORD previous = 0;
			// Execute stays on: a wrapper's table can share a page with code another thread
			// is running.
			if (!VirtualProtect(&a_table[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) {
				return false;
			}
			*a_original = a_table[a_slot];
			a_table[a_slot] = a_detour;
			VirtualProtect(&a_table[a_slot], sizeof(void*), previous, &previous);
			return true;
		}

		HRESULT STDMETHODCALLTYPE CreateVertexShaderDetour(ID3D11Device* a_self,
			const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage,
			ID3D11VertexShader** a_shader)
		{
			const HRESULT hr = g_original(a_self, a_bytecode, a_length, a_linkage, a_shader);

			if (FAILED(hr) || !a_shader || !*a_shader || !a_bytecode || a_length == 0 ||
				a_self != g_rendererDevice.load(std::memory_order_relaxed)) {
				return hr;
			}

			auto signature = Reflection::OutputSignatureChunk(a_bytecode, a_length);
			if (signature.empty()) {
				return hr;
			}

			const std::unique_lock lock(g_lock);
			const auto [it, inserted] = g_signatures.try_emplace(*a_shader);
			if (inserted) {
				// Holding a reference keeps the pointer from being recycled for a different
				// shader while the entry exists.
				(*a_shader)->AddRef();
			} else {
				g_bytes -= it->second.size();
			}
			g_bytes += signature.size();
			it->second = std::move(signature);

			return hr;
		}

		void STDMETHODCALLTYPE DrawIndexedDetour(ID3D11DeviceContext* a_context, UINT a_count,
			UINT a_start, INT a_base)
		{
			const auto original = reinterpret_cast<DrawIndexedFn>(HooksFor(a_context).nativeTable[kDrawIndexedSlot]);
			Hooks::NoteDrawContext(a_context);
			if (Tessellation::Armed() && Tessellation::BeforeDraw(a_context)) {
				original(a_context, a_count, a_start, a_base);
				Tessellation::AfterDraw(a_context);
				return;
			}
			original(a_context, a_count, a_start, a_base);
		}

		void STDMETHODCALLTYPE DrawIndexedInstancedDetour(ID3D11DeviceContext* a_context,
			UINT a_count, UINT a_instances, UINT a_start, INT a_base, UINT a_startInstance)
		{
			const auto original = reinterpret_cast<DrawIndexedInstancedFn>(HooksFor(a_context).nativeTable[kDrawIndexedInstancedSlot]);
			Hooks::NoteDrawContext(a_context);
			if (Tessellation::Armed() && Tessellation::BeforeDraw(a_context)) {
				original(a_context, a_count, a_instances, a_start, a_base, a_startInstance);
				Tessellation::AfterDraw(a_context);
				return;
			}
			original(a_context, a_count, a_instances, a_start, a_base, a_startInstance);
		}

		// Patches one table slot unless it already holds the detour (a second install, or
		// a table shared by another context): saving the detour as the original would make
		// the detour call itself.
		template <class Fn>
		bool HookSlot(void** a_table, size_t a_slot, void* a_detour, Fn& a_original)
		{
			if (a_original || a_table[a_slot] == a_detour) {
				return a_original != nullptr;
			}
			return PatchSlot(a_table, a_slot, a_detour, reinterpret_cast<void**>(&a_original));
		}

		bool InstallContextHooks(ID3D11DeviceContext* a_context)
		{
			auto** table = *reinterpret_cast<void***>(a_context);
			const std::unique_lock lock(g_contextLock);
			// Keep a later plugin's table in place when this exact live object already
			// owns our hooks. A reused address has no private-data cookie.
			if (ContextCookie(a_context)) return true;
			if (const auto found = g_contextHooks.find(table); found != g_contextHooks.end()) {
				const auto* hooks = &found->second;
				return SUCCEEDED(a_context->SetPrivateData(kContextHooksCookie, sizeof(hooks), &hooks));
			}

			// The runtime rewrites its own dispatch table as validation/fast paths change.
			// A private copy keeps our slots stable. Preserve every interface method on
			// this exact COM pointer, including extensions the engine may query later.
			size_t slots = 115; // ID3D11DeviceContext, through FinishCommandList.
			const auto extension = [&]<class T>(size_t count) {
				T* queried = nullptr;
				if (SUCCEEDED(a_context->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&queried)))) {
					if (static_cast<void*>(queried) == static_cast<void*>(a_context)) slots = std::max(slots, count);
					queried->Release();
				}
			};
			extension.operator()<ID3D11DeviceContext1>(134);
			extension.operator()<ID3D11DeviceContext2>(144);
			extension.operator()<ID3D11DeviceContext3>(147);
			extension.operator()<ID3D11DeviceContext4>(149);
			const auto attach = [&](const ContextHooks& a_hooks) {
				DWORD previous = 0;
				if (!VirtualProtect(a_context, sizeof(void*), PAGE_READWRITE, &previous)) {
					logger::error("ShaderRegistry: could not install the context draw table");
					return false;
				}
				const auto* cookie = &a_hooks;
				const bool saved = SUCCEEDED(a_context->SetPrivateData(kContextHooksCookie, sizeof(cookie), &cookie));
				if (saved) {
					InterlockedExchangePointer(reinterpret_cast<void* volatile*>(a_context), a_hooks.table.get());
				}
				VirtualProtect(a_context, sizeof(void*), previous, &previous);
				if (!saved) {
					logger::error("ShaderRegistry: could not record the context hook lifetime");
				}
				return saved;
			};
			// Contexts with the same runtime table can share its forwarding table. Keep
			// process-lifetime allocations bounded by table variants, not context churn.
			for (const auto& entry : g_contextHooks) {
				if (entry.second.nativeTable == table && entry.second.slots >= slots) {
					return attach(entry.second);
				}
			}
			ContextHooks hooks{};
			hooks.nativeTable = table;
			hooks.slots = slots;
			hooks.table = std::make_unique<void*[]>(slots);
			// Forward untouched methods through the runtime's LIVE table, so copying it
			// does not freeze startup validation paths or bypass later driver fast paths.
			// Windows x64: mov rax, &nativeTable[slot]; jmp qword ptr [rax]. RAX is volatile;
			// all COM arguments, the caller's return address and stack remain unchanged.
			constexpr size_t stride = 12;
			auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, slots * stride,
				MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (!code) { logger::error("ShaderRegistry: could not allocate context forwarders"); return false; }
			for (size_t slot = 0; slot < slots; ++slot) {
				auto* stub = code + slot * stride;
				stub[0] = 0x48; stub[1] = 0xB8;
				const auto address = reinterpret_cast<uintptr_t>(&table[slot]);
				std::memcpy(stub + 2, &address, sizeof(address));
				stub[10] = 0xFF; stub[11] = 0x20;
				hooks.table[slot] = stub;
			}
			DWORD codeProtection = 0;
			if (!VirtualProtect(code, slots * stride, PAGE_EXECUTE_READ, &codeProtection)) {
				VirtualFree(code, 0, MEM_RELEASE);
				logger::error("ShaderRegistry: could not make context forwarders executable"); return false;
			}
			FlushInstructionCache(GetCurrentProcess(), code, slots * stride);
			hooks.forwarders = code;
			hooks.table[kDrawIndexedSlot] = reinterpret_cast<void*>(&DrawIndexedDetour);
			hooks.table[kDrawIndexedInstancedSlot] = reinterpret_cast<void*>(&DrawIndexedInstancedDetour);
			auto** replacement = hooks.table.get();
			const auto installed = g_contextHooks.emplace(replacement, std::move(hooks)).first;
			if (!attach(installed->second)) {
				VirtualFree(installed->second.forwarders, 0, MEM_RELEASE);
				g_contextHooks.erase(installed);
				return false;
			}
			logger::info("ShaderRegistry: hooked {} context indexed draws ({} slots)",
				a_context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate", slots);
			return true;
		}

		HRESULT STDMETHODCALLTYPE CreateDeferredContextDetour(ID3D11Device* a_device, UINT a_flags,
			ID3D11DeviceContext** a_context)
		{
			const auto hr = g_createDeferredContext(a_device, a_flags, a_context);
			if (SUCCEEDED(hr) && a_context && *a_context &&
				a_device == g_rendererDevice.load(std::memory_order_relaxed)) {
				InstallContextHooks(*a_context);
			}
			return hr;
		}

		void InstallDrawHooks(ID3D11Device* a_device)
		{
			ID3D11DeviceContext* context = nullptr;
			a_device->GetImmediateContext(&context);
			if (context) {
				InstallContextHooks(context);
				if (!globals::d3d::context) {
					globals::d3d::context = context;
				}
				context->Release();
			}
			auto** table = *reinterpret_cast<void***>(a_device);
			if (!HookSlot(table, kCreateDeferredContextSlot,
				reinterpret_cast<void*>(&CreateDeferredContextDetour), g_createDeferredContext)) {
				logger::error("ShaderRegistry: could not watch deferred context creation");
			}
		}
	}

	namespace
	{
		using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE,
			HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
			const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
			D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

		CreateDeviceAndSwapChainFn g_originalCreateDevice{ nullptr };

		HRESULT WINAPI CreateDeviceAndSwapChainDetour(IDXGIAdapter* a_adapter,
			D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
			const D3D_FEATURE_LEVEL* a_featureLevels, UINT a_featureLevelCount,
			UINT a_sdkVersion, const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc,
			IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
			D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
		{
			const HRESULT hr = g_originalCreateDevice(a_adapter, a_driverType, a_software,
				a_flags, a_featureLevels, a_featureLevelCount, a_sdkVersion,
				a_swapChainDesc, a_swapChain, a_device, a_featureLevel, a_context);

			// Capture/video tools may create another swapchain later. Its resources must
			// never replace the renderer's device or immediate context.
			if (SUCCEEDED(hr) && a_device && *a_device && !globals::d3d::device) {
				globals::d3d::device = *a_device;
				if (a_context && *a_context) {
					globals::d3d::context = *a_context;
				}
				Install(*a_device);
			}

			return hr;
		}

		// Fallout 4 VR hands its frames to OpenVR, and such titles often create the device
		// without a swapchain; both creation paths are watched.
		using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
			const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*,
			ID3D11DeviceContext**);

		CreateDeviceFn g_originalCreateDeviceOnly{ nullptr };

		HRESULT WINAPI CreateDeviceDetour(IDXGIAdapter* a_adapter, D3D_DRIVER_TYPE a_driverType,
			HMODULE a_software, UINT a_flags, const D3D_FEATURE_LEVEL* a_featureLevels,
			UINT a_featureLevelCount, UINT a_sdkVersion, ID3D11Device** a_device,
			D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
		{
			const HRESULT hr = g_originalCreateDeviceOnly(a_adapter, a_driverType, a_software,
				a_flags, a_featureLevels, a_featureLevelCount, a_sdkVersion, a_device,
				a_featureLevel, a_context);

			// Only the first device is the renderer's; later ones (a capture tool, a video
			// decoder) are left alone.
			if (SUCCEEDED(hr) && a_device && *a_device && !globals::d3d::device) {
				globals::d3d::device = *a_device;
				if (a_context && *a_context) {
					globals::d3d::context = *a_context;
				}
				Install(*a_device);
			}

			return hr;
		}

		bool PatchImport(const char* a_module, const char* a_function, void* a_detour,
			void** a_original)
		{
			auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
			if (!base) {
				return false;
			}

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}

			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}

			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (directory.VirtualAddress == 0) {
				return false;
			}

			for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
				 descriptor->Name != 0; ++descriptor) {
				const auto* name = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(name, a_module) != 0 || descriptor->OriginalFirstThunk == 0) {
					continue;
				}

				auto* named = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
				auto* bound = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

				for (; named->u1.AddressOfData != 0; ++named, ++bound) {
					if (IMAGE_SNAP_BY_ORDINAL(named->u1.Ordinal)) {
						continue;
					}

					const auto* imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + named->u1.AddressOfData);
					if (std::strcmp(imported->Name, a_function) != 0) {
						continue;
					}

					DWORD previous = 0;
					if (!VirtualProtect(&bound->u1.Function, sizeof(void*), PAGE_READWRITE, &previous)) {
						return false;
					}

					*a_original = reinterpret_cast<void*>(bound->u1.Function);
					bound->u1.Function = reinterpret_cast<ULONGLONG>(a_detour);

					VirtualProtect(&bound->u1.Function, sizeof(void*), previous, &previous);
					return true;
				}
			}

			return false;
		}
	}

	bool InstallEarly()
	{
		if (g_originalCreateDevice || g_originalCreateDeviceOnly) {
			return true;
		}

		const bool withSwapChain = PatchImport("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
			reinterpret_cast<void*>(&CreateDeviceAndSwapChainDetour),
			reinterpret_cast<void**>(&g_originalCreateDevice));
		const bool deviceOnly = PatchImport("d3d11.dll", "D3D11CreateDevice",
			reinterpret_cast<void*>(&CreateDeviceDetour),
			reinterpret_cast<void**>(&g_originalCreateDeviceOnly));

		logger::info("ShaderRegistry: watching device creation ({}{}{})",
			withSwapChain ? "D3D11CreateDeviceAndSwapChain" : "",
			withSwapChain && deviceOnly ? ", " : "",
			deviceOnly ? "D3D11CreateDevice" : "");
		return withSwapChain || deviceOnly;
	}

	bool Install(ID3D11Device* a_device)
	{
		if (!a_device) {
			return false;
		}

		const std::scoped_lock installLock(g_installLock);
		const auto renderer = g_rendererDevice.load(std::memory_order_relaxed);
		if (renderer && renderer != a_device) {
			return false;
		}
		g_rendererDevice.store(a_device, std::memory_order_relaxed);
		InstallDrawHooks(a_device);

		if (g_installed) {
			return true;
		}

		auto** vtable = *reinterpret_cast<void***>(a_device);
		if (!vtable) {
			return false;
		}

		{
			const std::unique_lock lock(g_lock);
			g_signatures.reserve(8192);
		}

		g_installed = PatchSlot(vtable, kCreateVertexShaderSlot,
			reinterpret_cast<void*>(&CreateVertexShaderDetour), reinterpret_cast<void**>(&g_original));
		if (!g_installed) {
			logger::error("ShaderRegistry: could not unprotect the device vtable");
		}
		return g_installed;
	}

	bool DrawHookInstalled()
	{
		const std::shared_lock lock(g_contextLock);
		return !g_contextHooks.empty() && g_createDeferredContext &&
			std::all_of(g_contextHooks.begin(), g_contextHooks.end(), [](const auto& entry) {
				return entry.second.nativeTable && entry.second.forwarders;
			});
	}

	std::vector<uint8_t> OutputSignature(ID3D11VertexShader* a_shader)
	{
		if (!a_shader) {
			return {};
		}

		const std::shared_lock lock(g_lock);
		const auto             it = g_signatures.find(a_shader);
		return it == g_signatures.end() ? std::vector<uint8_t>{} : it->second;
	}

	size_t Count()
	{
		const std::shared_lock lock(g_lock);
		return g_signatures.size();
	}

	size_t Bytes()
	{
		const std::shared_lock lock(g_lock);
		return g_bytes;
	}
}
