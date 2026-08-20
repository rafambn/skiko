@file:JsModule("./js-skiko-reexport-symbols.mjs")
@file:JsNonModule
@file:JsQualifier("api")
package org.jetbrains.skiko.wasm

import org.jetbrains.skiko.InternalSkikoApi
import org.jetbrains.skia.impl.NativePointer
import kotlin.js.Promise

@InternalSkikoApi
actual external val awaitSkiko: Promise<JsAny>

@InternalSkikoApi
actual external fun setWebGPUDevice(device: JsAny)

@InternalSkikoApi
actual external fun addWebGPUTexture(texture: JsAny): NativePointer
