package org.jetbrains.skia.gpu.graphite

import org.jetbrains.skia.ExternalSymbolName
import org.jetbrains.skia.impl.Managed
import org.jetbrains.skia.impl.Native.Companion.NullPointer
import org.jetbrains.skia.impl.InteropPointer
import org.jetbrains.skia.impl.NativePointer
import org.jetbrains.skia.impl.Stats
import org.jetbrains.skia.impl.interopScope
import org.jetbrains.skiko.ExperimentalSkikoApi

/**
 * Represents a backend-specific texture that can be used by Graphite.
 */
@ExperimentalSkikoApi
class BackendTexture internal constructor(ptr: NativePointer) : Managed(ptr, _FinalizerHolder.PTR) {
    /** Returns a copy of the compatibility information for this texture. */
    val textureInfo: TextureInfo
        get() {
            Stats.onNativeCall()
            val ptr = _nGetTextureInfo(nativePtr)
            check(ptr != NullPointer) { "Backend texture has no valid texture info" }
            return TextureInfo(ptr)
        }

    companion object {
        init {
            GraphiteLibrary.load()
        }

        /**
         * Creates a Graphite backend texture that wraps a Metal texture.
         *
         * @param width width of the texture in pixels.
         * @param height height of the texture in pixels.
         * @param texturePtr native pointer to the Metal texture to wrap.
         * @return a backend texture wrapping the supplied Metal texture.
         */
        fun makeMetal(width: Int, height: Int, texturePtr: NativePointer): BackendTexture {
            requireMetalSupport()
            require(texturePtr != NullPointer) { "Metal texture pointer is null" }
            require(width > 0 && height > 0) { "Texture dimensions must be positive" }
            Stats.onNativeCall()
            val ptr = _nMakeMetal(width, height, texturePtr)
            check(ptr != NullPointer) { "Failed to create a Graphite Metal backend texture" }
            return BackendTexture(ptr)
        }

        /**
         * Creates a Graphite backend texture from an Emscripten WebGPU JS handle.
         *
         * The handle is released by the native bridge after it has been imported.
         */
        fun makeDawn(textureHandle: NativePointer): BackendTexture {
            require(textureHandle != NullPointer) { "WebGPU texture handle is null" }
            Stats.onNativeCall()
            val ptr = _nMakeDawn(textureHandle)
            check(ptr != NullPointer) { "Failed to create a Graphite Dawn backend texture" }
            return BackendTexture(ptr)
        }

        /**
         * Creates a Graphite backend texture that wraps an existing `VkImage`.
         *
         * The image memory is assumed to be managed by the client (or by the driver, as is the case
         * for swapchain images), so no allocation info is passed to Skia.
         */
        fun makeVulkan(
            width: Int,
            height: Int,
            textureInfo: VulkanTextureInfo,
            imageLayout: Int,
            queueFamilyIndex: Int,
            imagePtr: NativePointer,
        ): BackendTexture {
            requireVulkanSupport()
            require(imagePtr != NullPointer) { "Vulkan image pointer is null" }
            require(width > 0 && height > 0) { "Texture dimensions must be positive" }
            Stats.onNativeCall()
            val ptr = interopScope {
                _nMakeVulkan(
                    width,
                    height,
                    toInterop(textureInfo.packToIntArray()),
                    imageLayout,
                    queueFamilyIndex,
                    imagePtr,
                )
            }
            check(ptr != NullPointer) { "Failed to create a Graphite Vulkan backend texture" }
            return BackendTexture(ptr)
        }

        /**
         * Creates a Graphite backend texture from raw Vulkan format and usage values.
         *
         * This overload keeps compatibility with callers written before [VulkanTextureInfo]
         * became part of the public API.
         */
        fun makeVulkan(
            width: Int,
            height: Int,
            format: Int,
            imageUsage: Int,
            imageLayout: Int,
            queueFamilyIndex: Int,
            imagePtr: NativePointer,
        ): BackendTexture {
            require(queueFamilyIndex >= 0) { "Vulkan queue family index must not be negative" }
            return makeVulkan(
                width = width,
                height = height,
                textureInfo = VulkanTextureInfo(
                    format = VulkanFormat(format),
                    imageUsageFlags = VulkanImageUsageFlags(imageUsage),
                ),
                imageLayout = imageLayout,
                queueFamilyIndex = queueFamilyIndex,
                imagePtr = imagePtr,
            )
        }
    }

    private object _FinalizerHolder {
        val PTR = _nGetBackendTextureFinalizer()
    }
}

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_BackendTexture__1nGetFinalizer")
private external fun _nGetBackendTextureFinalizer(): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_BackendTexture__1nGetTextureInfo")
private external fun _nGetTextureInfo(backendTexturePtr: NativePointer): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_BackendTexture__1nMakeMetal")
private external fun _nMakeMetal(width: Int, height: Int, texturePtr: NativePointer): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_BackendTexture__1nMakeDawn")
private external fun _nMakeDawn(textureHandle: NativePointer): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_BackendTexture__1nMakeVulkan")
private external fun _nMakeVulkan(
    width: Int,
    height: Int,
    textureInfo: InteropPointer,
    imageLayout: Int,
    queueFamilyIndex: Int,
    imagePtr: NativePointer,
): NativePointer
