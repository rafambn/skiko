package org.jetbrains.skiko.graphite

import java.awt.Canvas
import java.awt.Dimension
import java.awt.Graphics
import kotlin.math.roundToInt
import org.jetbrains.skiko.Library
import org.jetbrains.skiko.useDrawingSurfacePlatformInfo

/** An X11-backed Vulkan swapchain host for a JVM Graphite surface on Linux. */
public class GraphiteVulkanHost : Canvas() {
    private var nativeHandle: Long = 0L
    private var disposed = false

    init {
        preferredSize = Dimension(1, 1)
    }

    /** Creates the Vulkan instance and surface after AWT has attached this component to a peer. */
    @Synchronized
    public fun initialize(): Boolean {
        check(!disposed) { "GraphiteVulkanHost is disposed" }
        if (nativeHandle != 0L) return true
        if (!isDisplayable) return false

        nativeHandle = useDrawingSurfacePlatformInfo(::nativeCreate)
        return nativeHandle != 0L
    }

    /** Resizes or creates the swapchain in physical pixels. */
    @Synchronized
    public fun resize(width: Int, height: Int, scale: Float): Boolean {
        checkHandle()
        val pixelWidth = (width * scale).roundToInt().coerceAtLeast(1)
        val pixelHeight = (height * scale).roundToInt().coerceAtLeast(1)
        return nativeResize(nativeHandle, pixelWidth, pixelHeight)
    }

    /** Current AWT backing scale factor used to size the Vulkan swapchain in pixels. */
    public val scale: Float
        get() = graphicsConfiguration?.defaultTransform?.scaleX?.toFloat()?.coerceAtLeast(1f) ?: 1f

    /** Acquires the next swapchain image and returns its Vulkan image handle. */
    @Synchronized
    public fun nextDrawable(): Long {
        checkHandle()
        return nativeNextDrawable(nativeHandle)
    }

    /** Presents the image acquired by [nextDrawable]. */
    @Synchronized
    public fun present() {
        checkHandle()
        nativePresent(nativeHandle)
    }

    /** Releases an acquired image after a failed frame. */
    @Synchronized
    public fun dropDrawable() {
        if (nativeHandle != 0L) nativeDropDrawable(nativeHandle)
    }

    public val instancePointer: Long
        get() = checkedHandle { nativeInstance(nativeHandle) }

    public val physicalDevicePointer: Long
        get() = checkedHandle { nativePhysicalDevice(nativeHandle) }

    public val devicePointer: Long
        get() = checkedHandle { nativeDevice(nativeHandle) }

    public val queuePointer: Long
        get() = checkedHandle { nativeQueue(nativeHandle) }

    public val queueFamilyIndex: Int
        get() = checkedHandle { nativeQueueFamilyIndex(nativeHandle) }

    public val imageFormat: Int
        get() = checkedHandle { nativeImageFormat(nativeHandle) }

    public val imageUsage: Int
        get() = checkedHandle { nativeImageUsage(nativeHandle) }

    public val imageLayout: Int
        get() = checkedHandle { nativeImageLayout(nativeHandle) }

    public val pixelWidth: Int
        get() = checkedHandle { nativeWidth(nativeHandle) }

    public val pixelHeight: Int
        get() = checkedHandle { nativeHeight(nativeHandle) }

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

    private inline fun <T> checkedHandle(block: () -> T): T {
        checkHandle()
        return block()
    }

    private fun checkHandle() {
        check(!disposed) { "GraphiteVulkanHost is disposed" }
        check(nativeHandle != 0L) { "GraphiteVulkanHost is not initialized" }
    }

    private external fun nativeCreate(platformInfo: Long): Long

    private external fun nativeResize(handle: Long, width: Int, height: Int): Boolean

    private external fun nativeNextDrawable(handle: Long): Long

    private external fun nativePresent(handle: Long)

    private external fun nativeDropDrawable(handle: Long)

    private external fun nativeInstance(handle: Long): Long

    private external fun nativePhysicalDevice(handle: Long): Long

    private external fun nativeDevice(handle: Long): Long

    private external fun nativeQueue(handle: Long): Long

    private external fun nativeQueueFamilyIndex(handle: Long): Int

    private external fun nativeImageFormat(handle: Long): Int

    private external fun nativeImageUsage(handle: Long): Int

    private external fun nativeImageLayout(handle: Long): Int

    private external fun nativeWidth(handle: Long): Int

    private external fun nativeHeight(handle: Long): Int

    private external fun nativeDispose(handle: Long)

    private companion object {
        init {
            Library.load()
        }
    }
}
