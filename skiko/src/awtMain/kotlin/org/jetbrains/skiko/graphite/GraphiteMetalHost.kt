package org.jetbrains.skiko.graphite

import java.awt.Canvas
import java.awt.Dimension
import java.awt.Graphics
import org.jetbrains.skiko.Library
import org.jetbrains.skiko.useDrawingSurfacePlatformInfo

/** AWT component that owns a macOS CAMetalLayer for a JVM Graphite host. */
public class GraphiteMetalHost : Canvas() {
    private var nativeHandle: Long = 0L
    private var disposed = false

    init {
        preferredSize = Dimension(1, 1)
    }

    /** Creates the native layer after Swing has attached this component to a peer. */
    @Synchronized
    public fun initialize(): Boolean {
        check(!disposed) { "GraphiteMetalHost is disposed" }
        if (nativeHandle != 0L) return true
        if (!isDisplayable) return false

        val initialScale = scale
        nativeHandle = useDrawingSurfacePlatformInfo { platformInfo ->
            nativeCreate(platformInfo, initialScale)
        }
        return nativeHandle != 0L
    }

    /** Current backing scale factor used to size the Metal drawable in pixels. */
    public val scale: Float
        get() = graphicsConfiguration?.defaultTransform?.scaleX?.toFloat()?.coerceAtLeast(1f) ?: 1f

    /** Resizes the layer and its drawable without recreating the Graphite context. */
    @Synchronized
    public fun resize(width: Int, height: Int, scale: Float) {
        checkHandle()
        var offsetX = 0
        var offsetY = 0
        var current: java.awt.Component? = this
        while (current != null) {
            offsetX += current.x
            offsetY += current.y
            current = current.parent
        }
        nativeResize(nativeHandle, offsetX, offsetY, width, height, scale)
    }

    /** Acquires the next CAMetalLayer drawable and returns its MTLTexture pointer. */
    @Synchronized
    public fun nextDrawable(): Long {
        checkHandle()
        return nativeNextDrawable(nativeHandle)
    }

    /** Returns the retained MTLDevice pointer used to create the Graphite context. */
    public val devicePointer: Long
        get() {
            checkHandle()
            return nativeDevice(nativeHandle)
        }

    /** Returns the retained MTLCommandQueue pointer used to create the Graphite context. */
    public val queuePointer: Long
        get() {
            checkHandle()
            return nativeQueue(nativeHandle)
        }

    /** Presents the drawable acquired by [nextDrawable]. */
    @Synchronized
    public fun present() {
        checkHandle()
        nativePresent(nativeHandle)
    }

    /** Releases an acquired drawable after a failed frame. */
    @Synchronized
    public fun dropDrawable() {
        if (nativeHandle != 0L) nativeDropDrawable(nativeHandle)
    }

    @Synchronized
    public fun close() {
        if (nativeHandle != 0L) {
            nativeDispose(nativeHandle)
            nativeHandle = 0L
        }
        disposed = true
    }

    override fun paint(g: Graphics) = Unit

    override fun removeNotify() {
        close()
        super.removeNotify()
    }

    private fun checkHandle() {
        check(!disposed) { "GraphiteMetalHost is disposed" }
        check(nativeHandle != 0L) { "GraphiteMetalHost is not initialized" }
    }

    private external fun nativeCreate(platformInfo: Long, scale: Float): Long

    private external fun nativeResize(
        handle: Long,
        x: Int,
        y: Int,
        width: Int,
        height: Int,
        scale: Float,
    )

    private external fun nativeNextDrawable(handle: Long): Long

    private external fun nativeDevice(handle: Long): Long

    private external fun nativeQueue(handle: Long): Long

    private external fun nativePresent(handle: Long)

    private external fun nativeDropDrawable(handle: Long)

    private external fun nativeDispose(handle: Long)

    private companion object {
        init {
            Library.load()
        }
    }
}
