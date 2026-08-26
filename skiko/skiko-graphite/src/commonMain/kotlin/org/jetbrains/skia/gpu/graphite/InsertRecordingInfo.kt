package org.jetbrains.skia.gpu.graphite

import org.jetbrains.skia.IPoint
import org.jetbrains.skia.IRect
import org.jetbrains.skia.Surface
import org.jetbrains.skiko.ExperimentalSkikoApi

/**
 * Information passed when inserting a [Recording] into a [GraphiteContext].
 *
 * @param recording recording to insert.
 * @param targetSurface concrete target for a recording created with a deferred canvas.
 * @param targetTranslation integer translation applied while replaying into [targetSurface].
 * @param targetClip optional clip in the translated target space.
 * @param waitSemaphores semaphores for the GPU to wait on before executing the recording.
 * @param signalSemaphores semaphores for the GPU to signal after executing the recording.
 */
@ExperimentalSkikoApi
class InsertRecordingInfo(
    val recording: Recording,
    val targetSurface: Surface? = null,
    val targetTranslation: IPoint = IPoint(0, 0),
    val targetClip: IRect? = null,
    val waitSemaphores: Array<BackendSemaphore> = emptyArray(),
    val signalSemaphores: Array<BackendSemaphore> = emptyArray(),
)
