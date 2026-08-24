// This file is merged with skiko.mjs by emcc

export const loadedWasm = {
    _: {}
}

let skikoGl = null;
let skikoModule = null;
const wasmReadyCallbacks = [];

export const registerSkikoWasmReadyCallback = (callback) => {
    wasmReadyCallbacks.push(callback);
};

const extensionLoadPromises = new Map();

export const loadSkikoExtension = (extensionPath) => {
    if (extensionLoadPromises.has(extensionPath)) return extensionLoadPromises.get(extensionPath);
    const loadPromise = awaitSkikoCore.then(async (module) => {
        await module.loadDynamicLibrary(extensionPath, {
            loadAsync: true,
            global: true,
            nodelete: true,
            nodeJS: false
        });

        const sideModuleExports = module.LDSO.loadedLibsByName[extensionPath].exports;
        Object.assign(loadedWasm._, sideModuleExports);

    }).catch((error) => {
        extensionLoadPromises.delete(extensionPath);
        throw error;
    });

    extensionLoadPromises.set(extensionPath, loadPromise);
    return loadPromise;
};

const awaitSkikoCore = loadSkikoWASM().then((module) => {
    skikoModule = module;
    const originalLocateFile = module.locateFile;

    module.locateFile = (path, prefix) => {
        // If path is already an absolute URL, don't prepend scriptDirectory
        if (path.startsWith("http://") || path.startsWith("https://") || path.startsWith("blob:")) {
            return path;
        }
        if (typeof originalLocateFile === "function") {
            return originalLocateFile(path, prefix);
        }
        return new URL(path, prefix || import.meta.url).href;
    };
    loadedWasm._ = module.wasmExports;
    if (!module.wasmExports.memory) {
        module.wasmExports.memory = {
            get buffer() {
                return module.HEAPU8.buffer;
            }
        };
    }
    skikoGl = module.GL;
    return module;
});

export const awaitSkiko = awaitSkikoCore.then(async (module) => {
    await Promise.allSettled(
        wasmReadyCallbacks.map(callback => callback(module))
    );

    return module
});

export const setWebGPUDevice = (device) => {
    if (!skikoModule) {
        throw new Error("Skiko WebGPU runtime is not ready");
    }
    skikoModule.preinitializedWebGPUDevice = device;
};

export const addWebGPUTexture = (texture) => {
    if (!skikoModule) {
        throw new Error("Skiko WebGPU runtime is not ready");
    }
    return skikoModule.JsValStore.add(texture);
};

export const GL = new Proxy({}, {
    get(object, propName) {
        return skikoGl[propName];
    }
})
