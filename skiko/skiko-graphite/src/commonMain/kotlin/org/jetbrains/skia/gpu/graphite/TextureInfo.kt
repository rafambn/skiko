package org.jetbrains.skia.gpu.graphite

import org.jetbrains.skia.ExternalSymbolName
import org.jetbrains.skia.impl.Managed
import org.jetbrains.skia.impl.NativePointer
import org.jetbrains.skiko.ExperimentalSkikoApi

/** Immutable backend texture compatibility information used by deferred Graphite canvases. */
@ExperimentalSkikoApi
class TextureInfo internal constructor(ptr: NativePointer) : Managed(ptr, _FinalizerHolder.PTR) {
    private object _FinalizerHolder {
        val PTR = _nGetTextureInfoFinalizer()
    }
}

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_TextureInfo__1nGetFinalizer")
private external fun _nGetTextureInfoFinalizer(): NativePointer
