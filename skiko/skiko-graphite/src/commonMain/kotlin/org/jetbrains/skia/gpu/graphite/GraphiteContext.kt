package org.jetbrains.skia.gpu.graphite

import org.jetbrains.skia.ExternalSymbolName
import org.jetbrains.skia.impl.Managed
import org.jetbrains.skia.impl.NativePointer
import org.jetbrains.skia.impl.Stats
import org.jetbrains.skia.impl.reachabilityBarrier
import org.jetbrains.skiko.ExperimentalSkikoApi

/**
 * The main entry point for Graphite, responsible for managing and coordinating GPU resources.
 */
@ExperimentalSkikoApi
class GraphiteContext internal constructor(ptr: NativePointer) : Managed(ptr, _FinalizerHolder.PTR) {
    companion object {
        init {
            GraphiteLibrary.load()
        }

        /**
         * Creates a Graphite context that submits work to a Metal command queue.
         *
         * @param devicePtr native pointer to the Metal device.
         * @param queuePtr native pointer to the Metal command queue.
         * @return a Graphite context backed by Metal.
         */
        fun makeMetal(devicePtr: NativePointer, queuePtr: NativePointer): GraphiteContext {
            requireMetalSupport()
            require(devicePtr != NullPointer) { "Metal device pointer is null" }
            require(queuePtr != NullPointer) { "Metal queue pointer is null" }
            Stats.onNativeCall()
            val ptr = _nMakeMetal(devicePtr, queuePtr)
            check(ptr != NullPointer) { "Failed to create a Graphite Metal context" }
            return GraphiteContext(ptr)
        }

        /**
         * Creates a Graphite context backed by the browser's WebGPU device.
         *
         * The WebGPU device must be installed in Skiko's Emscripten module before
         * calling this method.
         */
        fun makeDawn(): GraphiteContext {
            Stats.onNativeCall()
            val ptr = _nMakeDawn()
            check(ptr != NullPointer) { "Failed to create a Graphite Dawn context" }
            return GraphiteContext(ptr)
        }

        /**
         * Creates a Graphite context backed by a Vulkan device and queue.
         *
         * The Vulkan instance, physical device, logical device, and queue must remain valid for
         * the lifetime of the returned context.
         */
        fun makeVulkan(
            instancePtr: NativePointer,
            physicalDevicePtr: NativePointer,
            devicePtr: NativePointer,
            queuePtr: NativePointer,
            queueFamilyIndex: Int,
        ): GraphiteContext {
            require(instancePtr != NullPointer) { "Vulkan instance pointer is null" }
            require(physicalDevicePtr != NullPointer) { "Vulkan physical device pointer is null" }
            require(devicePtr != NullPointer) { "Vulkan device pointer is null" }
            require(queuePtr != NullPointer) { "Vulkan queue pointer is null" }
            require(queueFamilyIndex >= 0) { "Vulkan queue family index must not be negative" }
            Stats.onNativeCall()
            val ptr = _nMakeVulkan(
                instancePtr,
                physicalDevicePtr,
                devicePtr,
                queuePtr,
                queueFamilyIndex,
            )
            check(ptr != NullPointer) { "Failed to create a Graphite Vulkan context" }
            return GraphiteContext(ptr)
        }
    }

    /**
     * Creates a [Recorder] that records drawing commands for this context.
     *
     * @return a new recorder.
     */
    fun makeRecorder(): Recorder {
        return try {
            Stats.onNativeCall()
            val ptr = _nMakeRecorder(nativePtr)
            check(ptr != NullPointer) { "Failed to create a Graphite recorder" }
            Recorder(ptr)
        } finally {
            reachabilityBarrier(this)
        }
    }

    /**
     * Adds a [recording] to this context's pending GPU work.
     *
     * The work is sent to the GPU by a subsequent call to [submit].
     *
     * @param recording recording to insert.
     */
    fun insertRecording(recording: Recording) {
        try {
            Stats.onNativeCall()
            _nInsertRecording(nativePtr, recording.nativePtr)
        } finally {
            reachabilityBarrier(this)
            reachabilityBarrier(recording)
        }
    }

    /**
     * Submits pending work to the GPU.
     *
     * @param syncCpu if `true`, waits for the submitted GPU work to finish before returning.
     */
    fun submit(syncCpu: Boolean = false) {
        try {
            Stats.onNativeCall()
            _nSubmit(nativePtr, syncCpu)
        } finally {
            reachabilityBarrier(this)
        }
    }

    /** Polls completion callbacks for GPU work without waiting for the CPU. */
    fun checkAsyncWorkCompletion() {
        try {
            Stats.onNativeCall()
            _nCheckAsyncWorkCompletion(nativePtr)
        } finally {
            reachabilityBarrier(this)
        }
    }

    /** Whether this context still owns at least one unfinished GPU submission. */
    val hasUnfinishedGpuWork: Boolean
        get() = try {
            Stats.onNativeCall()
            _nHasUnfinishedGpuWork(nativePtr)
        } finally {
            reachabilityBarrier(this)
        }

    private object _FinalizerHolder {
        val PTR = _nGetGraphiteContextFinalizer()
    }
}

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nGetFinalizer")
private external fun _nGetGraphiteContextFinalizer(): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeMetal")
private external fun _nMakeMetal(devicePtr: NativePointer, queuePtr: NativePointer): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeDawn")
private external fun _nMakeDawn(): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeVulkan")
private external fun _nMakeVulkan(
    instancePtr: NativePointer,
    physicalDevicePtr: NativePointer,
    devicePtr: NativePointer,
    queuePtr: NativePointer,
    queueFamilyIndex: Int,
): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeRecorder")
private external fun _nMakeRecorder(contextPtr: NativePointer): NativePointer

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nInsertRecording")
private external fun _nInsertRecording(contextPtr: NativePointer, recordingPtr: NativePointer)

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nSubmit")
private external fun _nSubmit(contextPtr: NativePointer, syncCpu: Boolean)

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nCheckAsyncWorkCompletion")
private external fun _nCheckAsyncWorkCompletion(contextPtr: NativePointer)

@ExternalSymbolName("org_jetbrains_skia_gpu_graphite_GraphiteContext__1nHasUnfinishedGpuWork")
private external fun _nHasUnfinishedGpuWork(contextPtr: NativePointer): Boolean
