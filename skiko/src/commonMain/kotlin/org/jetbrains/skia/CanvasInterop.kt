package org.jetbrains.skia

import org.jetbrains.skia.impl.NativePointer
import org.jetbrains.skiko.ExperimentalSkikoApi

/** Wraps a borrowed native canvas whose lifetime is retained by [owner]. */
@ExperimentalSkikoApi
fun Canvas.Companion.wrapBorrowed(nativePtr: NativePointer, owner: Any): Canvas =
    Canvas(nativePtr, managed = false, _owner = owner)
