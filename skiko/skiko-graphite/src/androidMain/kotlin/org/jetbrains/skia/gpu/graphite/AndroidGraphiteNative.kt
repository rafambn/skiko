@file:OptIn(org.jetbrains.skiko.ExperimentalSkikoApi::class)

package org.jetbrains.skia.gpu.graphite

import android.view.Surface

/**
 * Low-level Android host for a Vulkan-backed Graphite surface.
 *
 * The caller owns every handle returned by [create] and must release it with [dispose].
 */
public object AndroidGraphiteNative {
    init {
        GraphiteLibrary.load()
    }

    /** Creates a native Graphite engine, returning zero when initialization fails. */
    @JvmStatic
    public external fun create(useHardwareBuffer: Boolean): Long

    /** Attaches or detaches the Android surface used for presentation. */
    @JvmStatic
    public external fun setSurface(handle: Long, surface: Surface?, width: Int, height: Int): Boolean

    /** Acquires the next frame for drawing. */
    @JvmStatic
    public external fun beginFrame(handle: Long): Boolean

    /** Updates the presentation timestamp for the active frame. */
    @JvmStatic
    public external fun setFrameTimeNanos(handle: Long, frameTimeNanos: Long)

    /** Submits and presents the active frame. */
    @JvmStatic
    public external fun endFrame(handle: Long): Boolean

    /** Releases the native engine and all GPU resources owned by it. */
    @JvmStatic
    public external fun dispose(handle: Long)

    /** Creates a worker-owned recorder from this engine's Graphite context. */
    public fun makeRecorder(handle: Long): Recorder {
        val ptr = nMakeRecorder(handle)
        check(ptr != 0L) { "Could not create an Android Graphite recorder" }
        return Recorder(ptr)
    }

    /** Copies the current presentation target compatibility information. */
    public fun targetTextureInfo(handle: Long): TextureInfo {
        val ptr = nTargetTextureInfo(handle)
        check(ptr != 0L) { "Android Graphite target is not ready" }
        return TextureInfo(ptr)
    }

    /** Inserts a deferred recording into the active presentation surface. */
    public fun insertRecording(
        handle: Long,
        recording: Recording,
        translationX: Int,
        translationY: Int,
        clipLeft: Int,
        clipTop: Int,
        clipRight: Int,
        clipBottom: Int,
        hasClip: Boolean,
    ): Boolean = nInsertRecording(
        handle,
        recording.nativePtr,
        translationX,
        translationY,
        clipLeft,
        clipTop,
        clipRight,
        clipBottom,
        hasClip,
    )

    /** Clears the active frame with an ARGB color. */
    @JvmStatic
    public external fun clear(handle: Long, color: Int)

    /** Saves the current canvas state. */
    @JvmStatic
    public external fun save(handle: Long)

    /** Restores the previous canvas state. */
    @JvmStatic
    public external fun restore(handle: Long)

    /** Translates the current canvas transform. */
    @JvmStatic
    public external fun translate(handle: Long, x: Float, y: Float)

    /** Rotates the current canvas transform in degrees. */
    @JvmStatic
    public external fun rotate(handle: Long, degrees: Float)

    /** Concatenates a column-major 4x4 transform. */
    @JvmStatic
    public external fun concat(handle: Long, columnMajor: FloatArray)

    /** Intersects the current clip with a rectangle. */
    @JvmStatic
    public external fun clipRect(
        handle: Long,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        antiAlias: Boolean,
    )

    /** Starts a mutable path. */
    @JvmStatic
    public external fun beginPath(handle: Long)

    /** Moves the mutable path cursor. */
    @JvmStatic
    public external fun moveTo(handle: Long, x: Float, y: Float)

    /** Adds a line to the mutable path. */
    @JvmStatic
    public external fun lineTo(handle: Long, x: Float, y: Float)

    /** Closes the mutable path. */
    @JvmStatic
    public external fun closePath(handle: Long)

    /** Draws the current mutable path. */
    @JvmStatic
    public external fun drawPath(handle: Long, color: Int, antiAlias: Boolean)

    /** Draws an immutable encoded path. */
    @JvmStatic
    public external fun drawImmutablePath(
        handle: Long,
        verbs: ByteArray,
        points: FloatArray,
        weights: FloatArray,
        fillType: Int,
        color: Int,
        stroke: Boolean,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    /** Draws a rectangle. */
    @JvmStatic
    public external fun drawRect(
        handle: Long,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        color: Int,
        stroke: Boolean,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    /** Draws a rounded rectangle. */
    @JvmStatic
    public external fun drawRoundRect(
        handle: Long,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        radiusX: Float,
        radiusY: Float,
        color: Int,
        stroke: Boolean,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    /** Draws an oval. */
    @JvmStatic
    public external fun drawOval(
        handle: Long,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        color: Int,
        stroke: Boolean,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    /** Draws a circle. */
    @JvmStatic
    public external fun drawCircle(
        handle: Long,
        x: Float,
        y: Float,
        radius: Float,
        color: Int,
        stroke: Boolean,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    /** Draws a line. */
    @JvmStatic
    public external fun drawLine(
        handle: Long,
        x0: Float,
        y0: Float,
        x1: Float,
        y1: Float,
        color: Int,
        strokeWidth: Float,
        antiAlias: Boolean,
    )

    @JvmStatic
    private external fun nMakeRecorder(handle: Long): Long

    @JvmStatic
    private external fun nTargetTextureInfo(handle: Long): Long

    @JvmStatic
    private external fun nInsertRecording(
        handle: Long,
        recordingPtr: Long,
        translationX: Int,
        translationY: Int,
        clipLeft: Int,
        clipTop: Int,
        clipRight: Int,
        clipBottom: Int,
        hasClip: Boolean,
    ): Boolean
}
