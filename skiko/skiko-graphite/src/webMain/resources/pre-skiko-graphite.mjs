import {
    loadedWasm,
    loadSkikoExtension,
    registerSkikoWasmReadyCallback,
} from "./skiko.mjs";

let graphiteLoadPromise = null;
let graphiteLoaded = false;
const graphiteWasm = new URL("./skiko-graphite.wasm", import.meta.url).href;

const ensureGraphiteLoaded = () => {
    if (!graphiteLoadPromise) {
        graphiteLoadPromise = loadSkikoExtension(graphiteWasm).then(() => {
            graphiteLoaded = true;
        });
    }
    return graphiteLoadPromise;
};

registerSkikoWasmReadyCallback(() => ensureGraphiteLoaded());

export const isSideModuleLoaded = () => graphiteLoaded;
