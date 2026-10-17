// Exercises the production draw-hook installation on actual D3D11 contexts.
// Tessellation is a call-counting stub: no engine state or rendering is needed.
#include "PCH.h"
#include "Globals.h"
#include "ShaderRegistry.h"
#include "Tessellation.h"
#include <thread>
#include <unordered_set>

namespace {
	thread_local bool armed = false;
	thread_local ID3D11DeviceContext* notedContext = nullptr;
	std::atomic<unsigned> before{0}, after{0};
	using IndexedDraw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	IndexedDraw chainedDraw = nullptr;
	unsigned chainedCalls = 0;
	void STDMETHODCALLTYPE ChainedDraw(ID3D11DeviceContext* context, UINT count, UINT start, INT base)
	{
		++chainedCalls;
		chainedDraw(context, count, start, base);
	}
}
namespace Hooks {
	void NoteDrawContext(ID3D11DeviceContext* context) { notedContext = context; }
}
namespace Tessellation {
	bool Armed() { return armed; }
	bool BeforeDraw(ID3D11DeviceContext*) { ++before; return true; }
	void AfterDraw(ID3D11DeviceContext*) { ++after; }
}

int main()
{
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* immediate = nullptr;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &device, nullptr, &immediate))) {
		std::fputs("FAIL: D3D11 WARP device creation\n", stderr);
		return 1;
	}
	unsigned failed = 0, checks = 0;
	const auto check = [&](bool ok, const char* label) {
		++checks;
		std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
		if (!ok) ++failed;
	};
	check(ShaderRegistry::Install(device) && ShaderRegistry::DrawHookInstalled(), "production hooks install");
	ShaderRegistry::Install(device); // Must not save a detour as its own original.
	ID3D11DeviceContext* deferred = nullptr;
	check(SUCCEEDED(device->CreateDeferredContext(0, &deferred)), "deferred context creation");
	ID3D11DeviceContext* secondDeferred = nullptr;
	check(SUCCEEDED(device->CreateDeferredContext(0, &secondDeferred)), "second deferred context sharing its vtable");
	for (auto* context : { immediate, deferred, secondDeferred, immediate }) {
		if (!context) continue;
		const bool isDeferred = context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
		before = after = 0;
		armed = false;
		context->DrawIndexed(0, 0, 0);
		check(before == 0 && after == 0, isDeferred ? "unarmed deferred draw unchanged" : "unarmed immediate draw unchanged");
		armed = true;
		context->DrawIndexed(0, 0, 0);
		context->DrawIndexedInstanced(0, 0, 0, 0, 0);
		check(before == 2 && after == 2, isDeferred ? "both deferred indexed draws bracketed" : "both immediate indexed draws bracketed");
		check(notedContext == context, "target capture receives the context that issued the draw");
	}
	before = after = 0;
	if (deferred && secondDeferred) {
		std::thread first([&] {
			armed = true;
			for (unsigned i = 0; i < 100; ++i) deferred->DrawIndexed(0, 0, 0);
		});
		std::thread second([&] {
			armed = true;
			for (unsigned i = 0; i < 100; ++i) secondDeferred->DrawIndexedInstanced(0, 0, 0, 0, 0);
		});
		first.join(); second.join();
		if (before != 200 || after != 200) std::printf("Observed brackets: before=%u after=%u\n", before.load(), after.load());
		check(before == 200 && after == 200, "deferred contexts on separate threads keep their originals and bracket each draw");
	}
	// A later renderer may clone our table and chain its own draw hook. Lookup
	// must continue to resolve this context even though the vptr is no longer ours.
	{
		auto** original = *reinterpret_cast<void***>(immediate);
		std::array<void*, 115> chainedTable;
		std::copy_n(original, chainedTable.size(), chainedTable.data());
		chainedDraw = reinterpret_cast<IndexedDraw>(original[12]);
		chainedTable[12] = reinterpret_cast<void*>(&ChainedDraw);
		DWORD previous = 0;
		const bool writable = VirtualProtect(immediate, sizeof(void*), PAGE_READWRITE, &previous) != 0;
		bool chained = false;
		if (writable) {
			InterlockedExchangePointer(reinterpret_cast<void* volatile*>(immediate), chainedTable.data());
			before = after = 0;
			armed = true;
			try {
				// Re-installing must preserve the later renderer's chain on this live
				// object, rather than recording that chain as the native original.
				ShaderRegistry::Install(device);
				immediate->DrawIndexed(0, 0, 0);
				immediate->DrawIndexedInstanced(0, 0, 0, 0, 0);
				chained = chainedCalls == 1 && before == 2 && after == 2;
			} catch (const std::exception&) {}
			InterlockedExchangePointer(reinterpret_cast<void* volatile*>(immediate), original);
			VirtualProtect(immediate, sizeof(void*), previous, &previous);
		}
		check(chained, "a later renderer's cloned context table can chain both indexed detours");
	}
	// Destroyed contexts commonly reuse their allocation immediately. An address
	// alone must not classify the replacement as an already-hooked live object.
	{
		std::unordered_set<ID3D11DeviceContext*> addresses;
		std::unordered_set<void**> tables;
		unsigned reused = 0;
		bool routed = true, released = true;
		for (unsigned i = 0; i < 128; ++i) {
			ID3D11DeviceContext* context = nullptr;
			if (FAILED(device->CreateDeferredContext(0, &context)) || !context) {
				routed = false;
				break;
			}
			reused += !addresses.insert(context).second;
			tables.insert(*reinterpret_cast<void***>(context));
			before = after = 0;
			armed = true;
			context->DrawIndexed(0, 0, 0);
			context->DrawIndexedInstanced(0, 0, 0, 0, 0);
			routed = routed && before == 2 && after == 2;
			released = (context->Release() == 0) && released;
		}
		check(routed, "destroyed/recreated deferred contexts always receive both draw hooks");
		check(released, "hook metadata holds no deferred-context lifetime references");
		check(tables.size() <= 2, "deferred context churn reuses forwarding tables instead of leaking one per object");
		std::printf("Context churn: %u reused addresses, %zu forwarding table variants\n", reused, tables.size());
	}
	// D3D devices can share the patched native device vtable. A capture tool's
	// device must still forward normally without acquiring our context hooks.
	ID3D11Device* foreignDevice = nullptr;
	ID3D11DeviceContext* foreignContext = nullptr;
	const auto secondDevice = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &foreignDevice, nullptr, &foreignContext);
	check(SUCCEEDED(secondDevice), "secondary device creation");
	if (foreignDevice && foreignContext) {
		check(!ShaderRegistry::Install(foreignDevice) && globals::d3d::context == immediate,
			"secondary device cannot replace renderer hooks or context");
		ID3D11DeviceContext* foreignDeferred = nullptr;
		check(SUCCEEDED(foreignDevice->CreateDeferredContext(0, &foreignDeferred)),
			"secondary deferred context creation still forwards");
		before = after = 0;
		armed = true;
		foreignContext->DrawIndexed(0, 0, 0);
		if (foreignDeferred) foreignDeferred->DrawIndexed(0, 0, 0);
		check(before == 0 && after == 0, "secondary immediate and deferred draws stay unhooked");
		if (foreignDeferred) foreignDeferred->Release();
	}
	if (foreignContext) foreignContext->Release();
	if (foreignDevice) foreignDevice->Release();
	if (secondDeferred) secondDeferred->Release();
	if (deferred) deferred->Release();
	immediate->Release();
	device->Release();
	std::printf("%s (%u checks)\n", failed ? "DRAW HOOK TESTS FAILED" : "ALL DRAW HOOK TESTS PASS", checks);
	return failed ? 1 : 0;
}
