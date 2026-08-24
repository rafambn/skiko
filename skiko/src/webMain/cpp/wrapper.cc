#include <emscripten.h>
#include <emscripten/html5.h>
#include <atomic>
#include <cstdint>
#include <dlfcn.h>
#include "webgl/webgl1.h"

// We need to have any definition invoking GL-lib on a backend size - otherwise GL is not created on frontend (regardless of -l=GL flag)
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE createContext(char* id) {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(id, &attr);
    emscripten_webgl_make_context_current(ctx);
    return ctx;
}

#if defined(GRAPHITE_PTHREADS_EXPERIMENT)

namespace {

std::atomic<int> gGraphiteSideModuleStatus{0};

void graphite_side_module_loaded(void*, void*) {
    gGraphiteSideModuleStatus.store(2, std::memory_order_release);
}

void graphite_side_module_failed(void*) {
    gGraphiteSideModuleStatus.store(-1, std::memory_order_release);
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE int graphite_pthread_load_side_module() {
    int expected = 0;
    if (!gGraphiteSideModuleStatus.compare_exchange_strong(
                expected,
                1,
                std::memory_order_acq_rel)) {
        return expected;
    }
    emscripten_dlopen(
            "skiko-graphite.wasm",
            RTLD_NOW | RTLD_GLOBAL,
            nullptr,
            graphite_side_module_loaded,
            graphite_side_module_failed);
    return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int graphite_pthread_side_module_status() {
    return gGraphiteSideModuleStatus.load(std::memory_order_acquire);
}

// This bridge intentionally lives in the main module. Dynamic side modules do
// not install new EM_JS bodies when they are loaded, while every pthread gets
// a copy of the main module's JavaScript runtime.
EM_JS(void, graphite_pthread_request_device_js, (std::uintptr_t callback), {
    const finish = (result) => {
        console.log('Graphite pthread WebGPU initialization result', result);
        getWasmTableEntry(callback)(result);
    };
    const initialization = Promise.resolve().then(async () => {
        if (!navigator.gpu) {
            return 1;
        }

        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) {
            return 2;
        }

        const device = await adapter.requestDevice();
        const canvas = Module['canvas']
            ?? GL?.offscreenCanvases?.['graphite-canvas']?.offscreenCanvas;
        if (!canvas) {
            return 3;
        }

        const context = canvas.getContext('webgpu');
        if (!context) {
            return 4;
        }

        context.configure({
            device,
            format: navigator.gpu.getPreferredCanvasFormat(),
            alphaMode: 'premultiplied',
        });
        const legacyLimits = new Proxy(device.limits, {
            get(target, property) {
                if (property === 'maxInterStageShaderComponents') return 60;
                return Reflect.get(target, property, target);
            },
        });
        const emscriptenDevice = new Proxy(device, {
            get(target, property) {
                if (property === 'limits') return legacyLimits;
                const value = Reflect.get(target, property, target);
                return typeof value === 'function' ? value.bind(target) : value;
            },
        });
        Module['preinitializedWebGPUDevice'] = emscriptenDevice;
        Module['graphitePthreadCanvasContext'] = context;
        return 0;
    });
    initialization.then(finish, (error) => {
        console.error('Graphite pthread WebGPU initialization failed', error);
        finish(5);
    });
});

extern "C" EMSCRIPTEN_KEEPALIVE void graphite_pthread_request_device(
        std::uintptr_t callback) {
    graphite_pthread_request_device_js(callback);
}

EM_JS(std::uint32_t, graphite_pthread_current_texture_handle_js, (), {
    const context = Module['graphitePthreadCanvasContext'];
    if (!context) return 0;
    return JsValStore.add(context.getCurrentTexture());
});

extern "C" EMSCRIPTEN_KEEPALIVE std::uint32_t graphite_pthread_current_texture_handle() {
    return graphite_pthread_current_texture_handle_js();
}

#endif
