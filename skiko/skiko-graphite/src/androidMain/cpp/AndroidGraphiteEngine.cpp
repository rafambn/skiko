#include <jni.h>

#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "include/android/vk/AndroidVulkanMemoryAllocator.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkM44.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/android/graphite/SurfaceAndroid.h"
#include "include/gpu/graphite/BackendSemaphore.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"

namespace {

constexpr char kLogTag[] = "GraphiteSurface";

void logError(const char* message) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message);
}

void logVkError(const char* operation, VkResult result) {
    __android_log_print(
        ANDROID_LOG_ERROR,
        kLogTag,
        "%s failed with VkResult=%d",
        operation,
        static_cast<int>(result));
}

bool succeeded(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    logVkError(operation, result);
    return false;
}

constexpr size_t kFramesInFlight = 3;
constexpr char kAndroidHardwareBufferExtension[] =
    "VK_ANDROID_external_memory_android_hardware_buffer";
constexpr char kExternalMemoryExtension[] = "VK_KHR_external_memory";
constexpr char kDedicatedAllocationExtension[] = "VK_KHR_dedicated_allocation";
constexpr char kGetMemoryRequirements2Extension[] = "VK_KHR_get_memory_requirements2";
constexpr char kExternalFenceExtension[] = "VK_KHR_external_fence";
constexpr char kExternalFenceFdExtension[] = "VK_KHR_external_fence_fd";
constexpr char kSamplerYcbcrConversionExtension[] = "VK_KHR_sampler_ycbcr_conversion";

struct HardwareBufferState {
    ~HardwareBufferState() {
        std::lock_guard<std::mutex> lock(mutex);
        if (releaseFenceFd >= 0) close(releaseFenceFd);
    }

    void markSubmitted() {
        std::lock_guard<std::mutex> lock(mutex);
        available = false;
        if (releaseFenceFd >= 0) {
            close(releaseFenceFd);
            releaseFenceFd = -1;
        }
    }

    void markReleased(int fenceFd) {
        std::lock_guard<std::mutex> lock(mutex);
        if (releaseFenceFd >= 0) close(releaseFenceFd);
        releaseFenceFd = fenceFd;
        available = fenceFd < 0;
    }

    bool pollAvailable() {
        std::lock_guard<std::mutex> lock(mutex);
        if (available) return true;
        if (releaseFenceFd < 0) return false;
        pollfd descriptor{releaseFenceFd, POLLIN | POLLERR | POLLHUP, 0};
        if (poll(&descriptor, 1, 0) > 0) {
            close(releaseFenceFd);
            releaseFenceFd = -1;
            available = true;
        }
        return available;
    }

private:
    std::mutex mutex;
    int releaseFenceFd = -1;
    bool available = true;
};

struct HardwareBufferCallbackContext {
    std::shared_ptr<HardwareBufferState> state;
    ASurfaceControl* surfaceControl = nullptr;
};

using AHardwareBufferAllocateProc = int (*)(
    const AHardwareBuffer_Desc*,
    AHardwareBuffer**);
using AHardwareBufferIsSupportedProc = int (*)(const AHardwareBuffer_Desc*);
using AHardwareBufferReleaseProc = void (*)(AHardwareBuffer*);
using ASurfaceControlCreateFromWindowProc = ASurfaceControl* (*)(ANativeWindow*, const char*);
using ASurfaceControlReleaseProc = void (*)(ASurfaceControl*);
using ASurfaceTransactionCreateProc = ASurfaceTransaction* (*)();
using ASurfaceTransactionDeleteProc = void (*)(ASurfaceTransaction*);
using ASurfaceTransactionApplyProc = void (*)(ASurfaceTransaction*);
using ASurfaceTransactionSetBufferProc = void (*)(
    ASurfaceTransaction*,
    ASurfaceControl*,
    AHardwareBuffer*,
    int);
using SurfaceTransactionOnBufferReleaseProc = void (*)(void*, int);
using SurfaceTransactionOnCompleteProc = void (*)(void*, ASurfaceTransactionStats*);
using ASurfaceTransactionSetBufferWithReleaseProc = void (*)(
    ASurfaceTransaction*,
    ASurfaceControl*,
    AHardwareBuffer*,
    int,
    void*,
    SurfaceTransactionOnBufferReleaseProc);
using ASurfaceTransactionSetGeometryProc = void (*)(
    ASurfaceTransaction*,
    ASurfaceControl*,
    const ARect&,
    const ARect&,
    int32_t);
using ASurfaceTransactionSetOnCompleteProc = void (*)(
    ASurfaceTransaction*,
    void*,
    SurfaceTransactionOnCompleteProc);
using ASurfaceTransactionSetEnableBackPressureProc = void (*)(
    ASurfaceTransaction*,
    ASurfaceControl*,
    bool);
using ASurfaceTransactionSetDesiredPresentTimeProc = void (*)(
    ASurfaceTransaction*,
    int64_t);
using ASurfaceTransactionStatsGetPreviousReleaseFenceFdProc = int (*)(
    ASurfaceTransactionStats*,
    ASurfaceControl*);

template <typename Proc>
Proc resolveAndroidProc(const char* name) {
    static void* androidLibrary = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    return androidLibrary == nullptr
               ? nullptr
               : reinterpret_cast<Proc>(dlsym(androidLibrary, name));
}

struct AndroidHardwareBufferApi {
    AHardwareBufferAllocateProc allocate =
        resolveAndroidProc<AHardwareBufferAllocateProc>("AHardwareBuffer_allocate");
    AHardwareBufferIsSupportedProc isSupported =
        resolveAndroidProc<AHardwareBufferIsSupportedProc>("AHardwareBuffer_isSupported");
    AHardwareBufferReleaseProc release =
        resolveAndroidProc<AHardwareBufferReleaseProc>("AHardwareBuffer_release");
    ASurfaceControlCreateFromWindowProc createFromWindow =
        resolveAndroidProc<ASurfaceControlCreateFromWindowProc>(
            "ASurfaceControl_createFromWindow");
    ASurfaceControlReleaseProc releaseSurfaceControl =
        resolveAndroidProc<ASurfaceControlReleaseProc>("ASurfaceControl_release");
    ASurfaceTransactionCreateProc createTransaction =
        resolveAndroidProc<ASurfaceTransactionCreateProc>("ASurfaceTransaction_create");
    ASurfaceTransactionDeleteProc deleteTransaction =
        resolveAndroidProc<ASurfaceTransactionDeleteProc>("ASurfaceTransaction_delete");
    ASurfaceTransactionApplyProc applyTransaction =
        resolveAndroidProc<ASurfaceTransactionApplyProc>("ASurfaceTransaction_apply");
    ASurfaceTransactionSetBufferProc setBuffer =
        resolveAndroidProc<ASurfaceTransactionSetBufferProc>("ASurfaceTransaction_setBuffer");
    ASurfaceTransactionSetBufferWithReleaseProc setBufferWithRelease =
        resolveAndroidProc<ASurfaceTransactionSetBufferWithReleaseProc>(
            "ASurfaceTransaction_setBufferWithRelease");
    ASurfaceTransactionSetGeometryProc setGeometry =
        resolveAndroidProc<ASurfaceTransactionSetGeometryProc>(
            "ASurfaceTransaction_setGeometry");
    ASurfaceTransactionSetOnCompleteProc setOnComplete =
        resolveAndroidProc<ASurfaceTransactionSetOnCompleteProc>(
            "ASurfaceTransaction_setOnComplete");
    ASurfaceTransactionSetEnableBackPressureProc setEnableBackPressure =
        resolveAndroidProc<ASurfaceTransactionSetEnableBackPressureProc>(
            "ASurfaceTransaction_setEnableBackPressure");
    ASurfaceTransactionSetDesiredPresentTimeProc setDesiredPresentTime =
        resolveAndroidProc<ASurfaceTransactionSetDesiredPresentTimeProc>(
            "ASurfaceTransaction_setDesiredPresentTime");
    ASurfaceTransactionStatsGetPreviousReleaseFenceFdProc getPreviousReleaseFenceFd =
        resolveAndroidProc<ASurfaceTransactionStatsGetPreviousReleaseFenceFdProc>(
            "ASurfaceTransactionStats_getPreviousReleaseFenceFd");

    bool available() const {
        return allocate != nullptr && isSupported != nullptr && release != nullptr &&
               createFromWindow != nullptr && releaseSurfaceControl != nullptr &&
               createTransaction != nullptr && deleteTransaction != nullptr &&
               applyTransaction != nullptr && setBuffer != nullptr && setGeometry != nullptr &&
               setOnComplete != nullptr;
    }
};

AndroidHardwareBufferApi& androidHardwareBufferApi() {
    static AndroidHardwareBufferApi api;
    return api;
}

void onHardwareBufferTransactionComplete(
        void* opaqueContext,
        ASurfaceTransactionStats* transactionStats) {
    std::unique_ptr<HardwareBufferCallbackContext> callbackContext(
        static_cast<HardwareBufferCallbackContext*>(opaqueContext));
    int releaseFenceFd = -1;
    AndroidHardwareBufferApi& androidApi = androidHardwareBufferApi();
    if (transactionStats != nullptr && callbackContext->surfaceControl != nullptr &&
        androidApi.getPreviousReleaseFenceFd != nullptr) {
        releaseFenceFd = androidApi.getPreviousReleaseFenceFd(
            transactionStats,
            callbackContext->surfaceControl);
    }
    if (callbackContext->state != nullptr) {
        callbackContext->state->markReleased(releaseFenceFd);
    }
}

void onHardwareBufferRelease(void* opaqueContext, int releaseFenceFd) {
    std::unique_ptr<HardwareBufferCallbackContext> callbackContext(
        static_cast<HardwareBufferCallbackContext*>(opaqueContext));
    if (callbackContext->state != nullptr) {
        callbackContext->state->markReleased(releaseFenceFd);
    }
}

struct FrameSlot {
    std::unique_ptr<skgpu::graphite::Recorder> recorder;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore completionSemaphore = VK_NULL_HANDLE;
    VkFence completionFence = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    bool inFlight = false;
    AHardwareBuffer* hardwareBuffer = nullptr;
    std::shared_ptr<HardwareBufferState> hardwareBufferState;
    skgpu::graphite::BackendTexture hardwareBackendTexture;
    sk_sp<SkSurface> hardwareSurface;
};

bool hasDeviceExtension(VkPhysicalDevice physicalDevice, const char* requestedExtension) {
    uint32_t propertyCount = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &propertyCount, nullptr) !=
        VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(propertyCount);
    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice,
            nullptr,
            &propertyCount,
            properties.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(
        properties.begin(),
        properties.end(),
        [requestedExtension](const VkExtensionProperties& property) {
            return std::string(property.extensionName) == requestedExtension;
        });
}

class GraphiteEngine final {
public:
    explicit GraphiteEngine(bool useHardwareBuffer) : useHardwareBuffer_(useHardwareBuffer) {}

    ~GraphiteEngine() {
        dispose();
    }

    bool initialize() {
        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "GraphiteSurface Android POC";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.pEngineName = "GraphiteSurface";
        applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_1;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &applicationInfo;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = extensions;

        if (!succeeded(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance")) {
            return false;
        }
        return true;
    }

    bool setSurface(JNIEnv* env, jobject javaSurface, int requestedWidth, int requestedHeight) {
        if (javaSurface == nullptr) {
            clearSurface();
            return true;
        }

        ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, javaSurface);
        if (nativeWindow == nullptr) {
            logError("ANativeWindow_fromSurface returned null");
            return false;
        }

        clearSurface();
        window_ = nativeWindow;

        VkAndroidSurfaceCreateInfoKHR surfaceInfo{
            VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.window = window_;
        if (!succeeded(
                vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_),
                "vkCreateAndroidSurfaceKHR")) {
            clearSurface();
            return false;
        }

        if (device_ == VK_NULL_HANDLE && !initializeDevice()) {
            clearSurface();
            return false;
        }

        if (hardwareBufferOutput_) {
            if (createHardwareBufferOutput(requestedWidth, requestedHeight)) return true;
            logError("AHardwareBuffer output could not be created; falling back to swapchain");
            destroyHardwareBufferOutput();
            hardwareBufferOutput_ = false;
        }
        return createSwapchain(requestedWidth, requestedHeight);
    }

    void setFrameTimeNanos(int64_t frameTimeNanos) {
        frameTimeNanos_ = frameTimeNanos;
    }

    bool beginFrame() {
        if (context_ == nullptr) return false;
        if (hardwareBufferOutput_) return beginHardwareBufferFrame();
        if (swapchain_ == VK_NULL_HANDLE) return false;

        if (swapchainOutOfDate_) {
            destroySwapchain();
            if (!createSwapchain(surfaceWidth_, surfaceHeight_)) return false;
        }

        pollFrameCompletions();
        const int frameSlotIndex = findAvailableFrameSlot();
        if (frameSlotIndex < 0) return false;
        FrameSlot& frameSlot = frameSlots_[static_cast<size_t>(frameSlotIndex)];

        uint32_t imageIndex = 0;
        // Keep acquire asynchronous, but give the presentation queue a short window to hand
        // back an image instead of dropping the entire display tick on VK_NOT_READY.
        VkResult result = vkAcquireNextImageKHR(
            device_,
            swapchain_,
            2'000'000,
            frameSlot.imageAvailableSemaphore,
            VK_NULL_HANDLE,
            &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainOutOfDate_ = true;
            return false;
        }
        if (result == VK_NOT_READY || result == VK_TIMEOUT) {
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            logVkError("vkAcquireNextImageKHR", result);
            return false;
        }

        if (imageInFlightFrameSlots_[imageIndex] >= 0 &&
            imageInFlightFrameSlots_[imageIndex] != frameSlotIndex) {
            const int previousFrameSlot = imageInFlightFrameSlots_[imageIndex];
            if (!waitForFrameSlot(previousFrameSlot)) return false;
        }

        activeFrameSlotIndex_ = frameSlotIndex;
        currentImageIndex_ = imageIndex;
        const VkExtent2D extent = swapchainExtent_;
        skgpu::graphite::VulkanTextureInfo textureInfo(
            VK_SAMPLE_COUNT_1_BIT,
            skgpu::Mipmapped::kNo,
            0,
            swapchainFormat_,
            VK_IMAGE_TILING_OPTIMAL,
            swapchainImageUsage_,
            VK_SHARING_MODE_EXCLUSIVE,
            VK_IMAGE_ASPECT_COLOR_BIT,
            {});
        const auto backendTexture = skgpu::graphite::BackendTextures::MakeVulkan(
            {static_cast<int>(extent.width), static_cast<int>(extent.height)},
            textureInfo,
            imageLayouts_[currentImageIndex_],
            queueFamilyIndex_,
            swapchainImages_[currentImageIndex_],
            {});
        frameSurface_ = SkSurfaces::WrapBackendTexture(
            frameSlot.recorder.get(),
            backendTexture,
            srgbColorSpace_,
            nullptr);
        if (frameSurface_ == nullptr) {
            resetActiveFrame();
            logError("SkSurfaces::WrapBackendTexture returned null");
            return false;
        }
        canvas_ = frameSurface_->getCanvas();
        if (canvas_ == nullptr) resetActiveFrame();
        return canvas_ != nullptr;
    }

    bool endFrame() {
        if (frameSurface_ == nullptr || canvas_ == nullptr) return false;
        if (hardwareBufferOutput_) return endHardwareBufferFrame();

        if (activeFrameSlotIndex_ < 0) return false;
        FrameSlot& frameSlot = frameSlots_[static_cast<size_t>(activeFrameSlotIndex_)];
        std::unique_ptr<skgpu::graphite::Recording> recording = frameSlot.recorder->snap();
        if (recording == nullptr) {
            resetActiveFrame();
            logError("Graphite recorder returned no recording");
            return false;
        }

        skgpu::graphite::BackendSemaphore waitSemaphore =
            skgpu::graphite::BackendSemaphores::MakeVulkan(frameSlot.imageAvailableSemaphore);
        std::array<VkSemaphore, 2> signalSemaphores = {
            renderFinishedSemaphores_[currentImageIndex_],
            frameSlot.completionSemaphore,
        };
        std::array<skgpu::graphite::BackendSemaphore, 2> backendSignalSemaphores = {
            skgpu::graphite::BackendSemaphores::MakeVulkan(signalSemaphores[0]),
            skgpu::graphite::BackendSemaphores::MakeVulkan(signalSemaphores[1]),
        };
        skgpu::graphite::InsertRecordingInfo insertInfo;
        insertInfo.fRecording = recording.get();
        insertInfo.fNumWaitSemaphores = 1;
        insertInfo.fWaitSemaphores = &waitSemaphore;
        insertInfo.fNumSignalSemaphores = backendSignalSemaphores.size();
        insertInfo.fSignalSemaphores = backendSignalSemaphores.data();

        if (!context_->insertRecording(insertInfo)) {
            resetActiveFrame();
            logError("Graphite Context::insertRecording failed");
            return false;
        }
        skgpu::graphite::SubmitInfo submitInfo;
        submitInfo.fSync = skgpu::graphite::SyncToCpu::kNo;
        submitInfo.fMarkBoundary = skgpu::graphite::MarkFrameBoundary::kYes;
        submitInfo.fFrameID = nextFrameId_++;
        if (!context_->submit(submitInfo)) {
            resetActiveFrame();
            logError("Graphite Context::submit failed");
            return false;
        }

        // Graphite owns the primary submission, so use a tiny follow-up submission to obtain a
        // Vulkan fence without forcing Graphite itself to synchronize to the CPU. The render-finish
        // semaphore is reserved for presentation; the completion semaphore is reserved for this
        // frame slot's reuse tracking.
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo completionSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        completionSubmit.waitSemaphoreCount = 1;
        completionSubmit.pWaitSemaphores = &frameSlot.completionSemaphore;
        completionSubmit.pWaitDstStageMask = &waitStage;
        if (!succeeded(
                vkQueueSubmit(queue_, 1, &completionSubmit, frameSlot.completionFence),
                "vkQueueSubmit(frame completion)")) {
            vkQueueWaitIdle(queue_);
            resetActiveFrame();
            return false;
        }

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentImageIndex_];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &currentImageIndex_;
        const VkResult presentResult = vkQueuePresentKHR(queue_, &presentInfo);
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR &&
            presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
            logVkError("vkQueuePresentKHR", presentResult);
        }

        const int submittedFrameSlotIndex = activeFrameSlotIndex_;
        frameSlot.imageIndex = currentImageIndex_;
        frameSlot.inFlight = true;
        imageInFlightFrameSlots_[currentImageIndex_] = submittedFrameSlotIndex;
        imageLayouts_[currentImageIndex_] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        resetActiveFrame();
        nextFrameSlotIndex_ =
            (static_cast<size_t>(submittedFrameSlotIndex) + 1) % frameSlots_.size();
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) swapchainOutOfDate_ = true;
        return presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR ||
               presentResult == VK_ERROR_OUT_OF_DATE_KHR;
    }

    void dispose() {
        clearSurface();
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        for (FrameSlot& frameSlot : frameSlots_) {
            if (frameSlot.imageAvailableSemaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, frameSlot.imageAvailableSemaphore, nullptr);
                frameSlot.imageAvailableSemaphore = VK_NULL_HANDLE;
            }
            if (frameSlot.completionSemaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, frameSlot.completionSemaphore, nullptr);
                frameSlot.completionSemaphore = VK_NULL_HANDLE;
            }
            if (frameSlot.completionFence != VK_NULL_HANDLE) {
                vkDestroyFence(device_, frameSlot.completionFence, nullptr);
                frameSlot.completionFence = VK_NULL_HANDLE;
            }
            frameSlot.recorder.reset();
        }
        frameSlots_.clear();
        context_.reset();
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    void clear(uint32_t color) {
        if (canvas_ != nullptr) canvas_->clear(static_cast<SkColor>(color));
    }

    void save() {
        if (canvas_ != nullptr) canvas_->save();
    }

    void restore() {
        if (canvas_ != nullptr) canvas_->restore();
    }

    void translate(float x, float y) {
        if (canvas_ != nullptr) canvas_->translate(x, y);
    }

    void rotate(float degrees) {
        if (canvas_ != nullptr) canvas_->rotate(degrees);
    }

    void concat(const float columnMajor[16]) {
        if (canvas_ != nullptr) canvas_->concat(SkM44::ColMajor(columnMajor));
    }

    void clipRect(float left, float top, float right, float bottom, bool antiAlias) {
        if (canvas_ != nullptr) {
            canvas_->clipRect(SkRect::MakeLTRB(left, top, right, bottom), antiAlias);
        }
    }

    void beginPath() {
        pathBuilder_ = SkPathBuilder();
    }

    void moveTo(float x, float y) {
        pathBuilder_.moveTo(x, y);
    }

    void lineTo(float x, float y) {
        pathBuilder_.lineTo(x, y);
    }

    void closePath() {
        pathBuilder_.close();
    }

    void drawPath(uint32_t color, bool antiAlias) {
        if (canvas_ == nullptr) return;
        SkPaint paint = makePaint(color, false, 1.0f, antiAlias);
        canvas_->drawPath(pathBuilder_.detach(), paint);
    }

    void drawImmutablePath(
            const std::vector<uint8_t>& verbs,
            const std::vector<float>& points,
            uint32_t color,
            bool stroke,
            float strokeWidth,
            bool antiAlias) {
        if (canvas_ == nullptr) return;
        SkPathBuilder builder;
        size_t pointIndex = 0;
        for (uint8_t verb : verbs) {
            switch (verb) {
                case 1:
                    if (pointIndex + 2 > points.size()) return;
                    builder.moveTo(points[pointIndex], points[pointIndex + 1]);
                    pointIndex += 2;
                    break;
                case 2:
                    if (pointIndex + 2 > points.size()) return;
                    builder.lineTo(points[pointIndex], points[pointIndex + 1]);
                    pointIndex += 2;
                    break;
                case 3:
                    builder.close();
                    break;
                default:
                    return;
            }
        }
        if (pointIndex != points.size()) return;
        const SkPaint paint = makePaint(color, stroke, strokeWidth, antiAlias);
        canvas_->drawPath(builder.detach(), paint);
    }

    void drawRect(
            float left, float top, float right, float bottom,
            uint32_t color, bool stroke, float strokeWidth, bool antiAlias) {
        if (canvas_ == nullptr) return;
        const SkPaint paint = makePaint(color, stroke, strokeWidth, antiAlias);
        canvas_->drawRect(SkRect::MakeLTRB(left, top, right, bottom), paint);
    }

    void drawRoundRect(
            float left, float top, float right, float bottom, float radiusX, float radiusY,
            uint32_t color, bool stroke, float strokeWidth, bool antiAlias) {
        if (canvas_ == nullptr) return;
        const SkPaint paint = makePaint(color, stroke, strokeWidth, antiAlias);
        canvas_->drawRRect(
                SkRRect::MakeRectXY(SkRect::MakeLTRB(left, top, right, bottom), radiusX, radiusY),
                paint);
    }

    void drawOval(
            float left, float top, float right, float bottom,
            uint32_t color, bool stroke, float strokeWidth, bool antiAlias) {
        if (canvas_ == nullptr) return;
        const SkPaint paint = makePaint(color, stroke, strokeWidth, antiAlias);
        canvas_->drawOval(SkRect::MakeLTRB(left, top, right, bottom), paint);
    }

    void drawCircle(
            float x, float y, float radius,
            uint32_t color, bool stroke, float strokeWidth, bool antiAlias) {
        if (canvas_ == nullptr) return;
        const SkPaint paint = makePaint(color, stroke, strokeWidth, antiAlias);
        canvas_->drawCircle(x, y, radius, paint);
    }

    void drawLine(
            float x0, float y0, float x1, float y1,
            uint32_t color, float strokeWidth, bool antiAlias) {
        if (canvas_ == nullptr) return;
        const SkPaint paint = makePaint(color, true, strokeWidth, antiAlias);
        canvas_->drawLine(x0, y0, x1, y1, paint);
    }

private:
    static SkPaint makePaint(uint32_t color, bool stroke, float strokeWidth, bool antiAlias) {
        SkPaint paint;
        paint.setColor(static_cast<SkColor>(color));
        paint.setStyle(stroke ? SkPaint::kStroke_Style : SkPaint::kFill_Style);
        paint.setStrokeWidth(strokeWidth);
        paint.setAntiAlias(antiAlias);
        return paint;
    }

    int findAvailableFrameSlot() const {
        if (frameSlots_.empty()) return -1;
        for (size_t offset = 0; offset < frameSlots_.size(); ++offset) {
            const size_t index = (nextFrameSlotIndex_ + offset) % frameSlots_.size();
            if (!frameSlots_[index].inFlight) return static_cast<int>(index);
        }
        return -1;
    }

    void pollFrameCompletions() {
        if (context_ != nullptr) context_->checkAsyncWorkCompletion();
        if (hardwareBufferOutput_) {
            for (FrameSlot& frameSlot : frameSlots_) {
                if (frameSlot.inFlight && frameSlot.hardwareBufferState != nullptr &&
                    frameSlot.hardwareBufferState->pollAvailable()) {
                    frameSlot.inFlight = false;
                }
            }
            return;
        }
        for (size_t index = 0; index < frameSlots_.size(); ++index) {
            FrameSlot& frameSlot = frameSlots_[index];
            if (!frameSlot.inFlight) continue;
            const VkResult status = vkGetFenceStatus(device_, frameSlot.completionFence);
            if (status != VK_SUCCESS) continue;
            vkResetFences(device_, 1, &frameSlot.completionFence);
            frameSlot.inFlight = false;
            if (frameSlot.imageIndex < imageInFlightFrameSlots_.size() &&
                imageInFlightFrameSlots_[frameSlot.imageIndex] == static_cast<int>(index)) {
                imageInFlightFrameSlots_[frameSlot.imageIndex] = -1;
            }
        }
    }

    bool waitForFrameSlot(int frameSlotIndex) {
        if (frameSlotIndex < 0 || static_cast<size_t>(frameSlotIndex) >= frameSlots_.size()) {
            return false;
        }
        FrameSlot& frameSlot = frameSlots_[static_cast<size_t>(frameSlotIndex)];
        if (!frameSlot.inFlight) return true;
        if (!succeeded(
                vkWaitForFences(device_, 1, &frameSlot.completionFence, VK_TRUE, UINT64_MAX),
                "vkWaitForFences(frame slot)")) {
            return false;
        }
        vkResetFences(device_, 1, &frameSlot.completionFence);
        frameSlot.inFlight = false;
        if (frameSlot.imageIndex < imageInFlightFrameSlots_.size() &&
            imageInFlightFrameSlots_[frameSlot.imageIndex] == frameSlotIndex) {
            imageInFlightFrameSlots_[frameSlot.imageIndex] = -1;
        }
        if (context_ != nullptr) context_->checkAsyncWorkCompletion();
        return true;
    }

    void resetActiveFrame() {
        frameSurface_.reset();
        canvas_ = nullptr;
        activeFrameSlotIndex_ = -1;
    }

    void releaseHardwareBackendTexture(FrameSlot& frameSlot) {
        frameSlot.hardwareSurface.reset();
        if (frameSlot.hardwareBackendTexture.isValid() && frameSlot.recorder != nullptr) {
            frameSlot.recorder->deleteBackendTexture(frameSlot.hardwareBackendTexture);
            frameSlot.hardwareBackendTexture = {};
        }
    }

    bool beginHardwareBufferFrame() {
        if (hardwareSurfaceControl_ == nullptr) return false;
        pollFrameCompletions();
        const int frameSlotIndex = findAvailableFrameSlot();
        if (frameSlotIndex < 0) return false;
        FrameSlot& frameSlot = frameSlots_[static_cast<size_t>(frameSlotIndex)];
        if (frameSlot.hardwareSurface == nullptr ||
            !frameSlot.hardwareBackendTexture.isValid()) {
            logError("Graphite hardware-buffer resources are not initialized");
            return false;
        }
        activeFrameSlotIndex_ = frameSlotIndex;
        frameSurface_ = frameSlot.hardwareSurface;
        canvas_ = frameSurface_->getCanvas();
        if (canvas_ == nullptr) resetActiveFrame();
        return canvas_ != nullptr;
    }

    bool endHardwareBufferFrame() {
        if (activeFrameSlotIndex_ < 0) return false;
        FrameSlot& frameSlot = frameSlots_[static_cast<size_t>(activeFrameSlotIndex_)];
        std::unique_ptr<skgpu::graphite::Recording> recording = frameSlot.recorder->snap();
        if (recording == nullptr) {
            resetActiveFrame();
            logError("Graphite hardware-buffer recorder returned no recording");
            return false;
        }

        skgpu::graphite::BackendSemaphore signalSemaphore =
            skgpu::graphite::BackendSemaphores::MakeVulkan(frameSlot.completionSemaphore);
        skgpu::graphite::InsertRecordingInfo insertInfo;
        insertInfo.fRecording = recording.get();
        insertInfo.fNumSignalSemaphores = 1;
        insertInfo.fSignalSemaphores = &signalSemaphore;
        if (!context_->insertRecording(insertInfo)) {
            resetActiveFrame();
            logError("Graphite hardware-buffer recording could not be inserted");
            return false;
        }

        skgpu::graphite::SubmitInfo submitInfo;
        submitInfo.fSync = skgpu::graphite::SyncToCpu::kNo;
        submitInfo.fMarkBoundary = skgpu::graphite::MarkFrameBoundary::kYes;
        submitInfo.fFrameID = nextFrameId_++;
        if (!context_->submit(submitInfo)) {
            resetActiveFrame();
            logError("Graphite hardware-buffer submission failed");
            return false;
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo completionSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        completionSubmit.waitSemaphoreCount = 1;
        completionSubmit.pWaitSemaphores = &frameSlot.completionSemaphore;
        completionSubmit.pWaitDstStageMask = &waitStage;
        if (!succeeded(
                vkQueueSubmit(queue_, 1, &completionSubmit, frameSlot.completionFence),
                "vkQueueSubmit(hardware-buffer completion)")) {
            vkQueueWaitIdle(queue_);
            resetActiveFrame();
            return false;
        }

        auto getFenceFd = reinterpret_cast<PFN_vkGetFenceFdKHR>(
            vkGetDeviceProcAddr(device_, "vkGetFenceFdKHR"));
        if (getFenceFd == nullptr) {
            logError("VK_KHR_external_fence_fd is unavailable");
            vkQueueWaitIdle(queue_);
            resetActiveFrame();
            return false;
        }
        VkFenceGetFdInfoKHR fenceFdInfo{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
        fenceFdInfo.fence = frameSlot.completionFence;
        fenceFdInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        int acquireFenceFd = -1;
        if (!succeeded(
                getFenceFd(device_, &fenceFdInfo, &acquireFenceFd),
                "vkGetFenceFdKHR")) {
            vkQueueWaitIdle(queue_);
            resetActiveFrame();
            return false;
        }

        const int submittedFrameSlotIndex = activeFrameSlotIndex_;
        frameSlot.hardwareBufferState->markSubmitted();
        AndroidHardwareBufferApi& androidApi = androidHardwareBufferApi();
        const bool useBufferReleaseCallback =
            android_get_device_api_level() >= 36 &&
            androidApi.setBufferWithRelease != nullptr;
        std::shared_ptr<HardwareBufferState> callbackState =
            useBufferReleaseCallback ? frameSlot.hardwareBufferState : lastHardwareBufferState_;
        auto* callbackContext = new HardwareBufferCallbackContext{
            callbackState,
            useBufferReleaseCallback ? nullptr : hardwareSurfaceControl_,
        };
        ASurfaceTransaction* transaction = androidApi.createTransaction();
        if (transaction == nullptr) {
            close(acquireFenceFd);
            frameSlot.hardwareBufferState->markReleased(-1);
            delete callbackContext;
            vkQueueWaitIdle(queue_);
            resetActiveFrame();
            logError("ASurfaceTransaction_create returned null");
            return false;
        }

        if (useBufferReleaseCallback) {
            androidApi.setBufferWithRelease(
                transaction,
                hardwareSurfaceControl_,
                frameSlot.hardwareBuffer,
                acquireFenceFd,
                callbackContext,
                onHardwareBufferRelease);
        } else {
            androidApi.setBuffer(
                transaction,
                hardwareSurfaceControl_,
                frameSlot.hardwareBuffer,
                acquireFenceFd);
        }
        if (androidApi.setDesiredPresentTime != nullptr && frameTimeNanos_ > 0) {
            androidApi.setDesiredPresentTime(transaction, frameTimeNanos_);
        }
        if (androidApi.setEnableBackPressure != nullptr) {
            androidApi.setEnableBackPressure(
                transaction,
                hardwareSurfaceControl_,
                true);
        }
        if (!hardwareSurfaceGeometrySet_) {
            const ARect source{0, 0, surfaceWidth_, surfaceHeight_};
            const ARect destination{0, 0, surfaceWidth_, surfaceHeight_};
            androidApi.setGeometry(
                transaction,
                hardwareSurfaceControl_,
                source,
                destination,
                0);
        }
        if (useBufferReleaseCallback) {
            callbackContext = nullptr;
        } else {
            androidApi.setOnComplete(
                transaction,
                callbackContext,
                onHardwareBufferTransactionComplete);
        }
        androidApi.applyTransaction(transaction);
        androidApi.deleteTransaction(transaction);
        hardwareSurfaceGeometrySet_ = true;

        if (!useBufferReleaseCallback) {
            lastHardwareBufferState_ = frameSlot.hardwareBufferState;
        }
        frameSlot.inFlight = true;
        resetActiveFrame();
        nextFrameSlotIndex_ =
            (static_cast<size_t>(submittedFrameSlotIndex) + 1) % frameSlots_.size();
        return true;
    }

    bool createHardwareBufferOutput(int requestedWidth, int requestedHeight) {
        if (!hardwareBufferOutput_ || android_get_device_api_level() < 29) return false;
        AndroidHardwareBufferApi& androidApi = androidHardwareBufferApi();
        if (!androidApi.available()) return false;
        if (android_get_device_api_level() < 36 &&
            androidApi.getPreviousReleaseFenceFd == nullptr) {
            logError("SurfaceControl release-fence API is unavailable; using swapchain");
            return false;
        }
        hardwareSurfaceControl_ = androidApi.createFromWindow(
            window_,
            "GraphiteSurface-AHardwareBuffer");
        if (hardwareSurfaceControl_ == nullptr) return false;
        hardwareSurfaceGeometrySet_ = false;

        surfaceWidth_ = std::max(1, requestedWidth);
        surfaceHeight_ = std::max(1, requestedHeight);
        swapchainExtent_ = {
            static_cast<uint32_t>(surfaceWidth_),
            static_cast<uint32_t>(surfaceHeight_),
        };
        AHardwareBuffer_Desc description{};
        description.width = static_cast<uint32_t>(surfaceWidth_);
        description.height = static_cast<uint32_t>(surfaceHeight_);
        description.layers = 1;
        description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        description.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        if (!androidApi.isSupported(&description)) {
            description.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        }
        if (!androidApi.isSupported(&description)) return false;

        for (FrameSlot& frameSlot : frameSlots_) {
            if (androidApi.allocate(&description, &frameSlot.hardwareBuffer) != 0) {
                return false;
            }
            frameSlot.hardwareBufferState = std::make_shared<HardwareBufferState>();
            frameSlot.hardwareBackendTexture = frameSlot.recorder->createBackendTexture(
                frameSlot.hardwareBuffer,
                true,
                false,
                {surfaceWidth_, surfaceHeight_},
                false);
            if (!frameSlot.hardwareBackendTexture.isValid()) {
                logError("Graphite could not import the AHardwareBuffer");
                return false;
            }
            frameSlot.hardwareSurface = SkSurfaces::WrapBackendTexture(
                frameSlot.recorder.get(),
                frameSlot.hardwareBackendTexture,
                srgbColorSpace_,
                nullptr);
            if (frameSlot.hardwareSurface == nullptr) {
                logError("Graphite could not wrap the AHardwareBuffer");
                return false;
            }
        }
        return true;
    }

    void destroyHardwareBufferOutput() {
        if (hardwareSurfaceControl_ == nullptr &&
            std::none_of(
                frameSlots_.begin(),
                frameSlots_.end(),
                [](const FrameSlot& frameSlot) { return frameSlot.hardwareBuffer != nullptr; })) {
            return;
        }
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        resetActiveFrame();
        lastHardwareBufferState_.reset();
        AndroidHardwareBufferApi& androidApi = androidHardwareBufferApi();
        if (hardwareSurfaceControl_ != nullptr) {
            if (androidApi.releaseSurfaceControl != nullptr) {
                androidApi.releaseSurfaceControl(hardwareSurfaceControl_);
            }
            hardwareSurfaceControl_ = nullptr;
        }
        hardwareSurfaceGeometrySet_ = false;
        for (FrameSlot& frameSlot : frameSlots_) {
            releaseHardwareBackendTexture(frameSlot);
            if (frameSlot.hardwareBuffer != nullptr) {
                if (androidApi.release != nullptr) androidApi.release(frameSlot.hardwareBuffer);
                frameSlot.hardwareBuffer = nullptr;
            }
            frameSlot.hardwareBufferState.reset();
            frameSlot.inFlight = false;
        }
    }

    bool initializeDevice() {
        uint32_t physicalDeviceCount = 0;
        if (!succeeded(
                vkEnumeratePhysicalDevices(instance_, &physicalDeviceCount, nullptr),
                "vkEnumeratePhysicalDevices")) {
            return false;
        }
        if (physicalDeviceCount == 0) {
            logError("No Vulkan physical device is available");
            return false;
        }
        std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
        if (!succeeded(
                vkEnumeratePhysicalDevices(
                    instance_,
                    &physicalDeviceCount,
                    physicalDevices.data()),
                "vkEnumeratePhysicalDevices")) {
            return false;
        }

        const bool requestHardwareBuffer =
            useHardwareBuffer_ && android_get_device_api_level() >= 29;
        for (int selectionPass = 0; selectionPass < (requestHardwareBuffer ? 2 : 1);
             ++selectionPass) {
            const bool requireHardwareBuffer = requestHardwareBuffer && selectionPass == 0;
            for (VkPhysicalDevice candidate : physicalDevices) {
                if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
                if (requireHardwareBuffer &&
                    (!hasDeviceExtension(candidate, kAndroidHardwareBufferExtension) ||
                     !hasDeviceExtension(candidate, kExternalFenceFdExtension))) {
                    continue;
                }
                uint32_t queueFamilyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
                std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(
                    candidate,
                    &queueFamilyCount,
                    queueFamilies.data());
                for (uint32_t index = 0; index < queueFamilyCount; ++index) {
                    VkBool32 supportsPresent = VK_FALSE;
                    if (vkGetPhysicalDeviceSurfaceSupportKHR(
                            candidate,
                            index,
                            surface_,
                            &supportsPresent) != VK_SUCCESS) {
                        continue;
                    }
                    if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                        supportsPresent == VK_TRUE) {
                        physicalDevice_ = candidate;
                        queueFamilyIndex_ = index;
                        hardwareBufferOutput_ = requireHardwareBuffer;
                        break;
                    }
                }
                if (physicalDevice_ != VK_NULL_HANDLE) break;
            }
            if (physicalDevice_ != VK_NULL_HANDLE) break;
        }
        if (physicalDevice_ == VK_NULL_HANDLE) {
            logError("No Vulkan queue family supports graphics and presentation");
            return false;
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamilyIndex_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (hardwareBufferOutput_) {
            const char* optionalExtensions[] = {
                kAndroidHardwareBufferExtension,
                kExternalMemoryExtension,
                kDedicatedAllocationExtension,
                kGetMemoryRequirements2Extension,
                kExternalFenceExtension,
                kExternalFenceFdExtension,
                kSamplerYcbcrConversionExtension,
            };
            for (const char* extension : optionalExtensions) {
                if (hasDeviceExtension(physicalDevice_, extension)) {
                    deviceExtensions.push_back(extension);
                }
            }
        }
        VkPhysicalDeviceFeatures features{};
        VkPhysicalDeviceSamplerYcbcrConversionFeatures samplerYcbcrFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
        if (hardwareBufferOutput_) {
            VkPhysicalDeviceFeatures2 availableFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            availableFeatures.pNext = &samplerYcbcrFeatures;
            auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFeatures2"));
            if (getPhysicalDeviceFeatures2 == nullptr) {
                logError("Vulkan feature-query entry point is unavailable; using swapchain");
                hardwareBufferOutput_ = false;
            } else {
                getPhysicalDeviceFeatures2(physicalDevice_, &availableFeatures);
            }
            if (samplerYcbcrFeatures.samplerYcbcrConversion != VK_TRUE) {
                logError("Vulkan sampler YCbCr conversion is unavailable; using swapchain");
                hardwareBufferOutput_ = false;
            }
        }
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        if (hardwareBufferOutput_) {
            samplerYcbcrFeatures.samplerYcbcrConversion = VK_TRUE;
            deviceInfo.pNext = &samplerYcbcrFeatures;
            deviceInfo.pEnabledFeatures = nullptr;
        } else {
            deviceInfo.pEnabledFeatures = &features;
        }
        if (!succeeded(
                vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_),
                "vkCreateDevice")) {
            return false;
        }
        vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);

        if (hardwareBufferOutput_ &&
            vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID") ==
                nullptr) {
            logError("Vulkan AHardwareBuffer import entry points are unavailable; using swapchain");
            hardwareBufferOutput_ = false;
        }

        skgpu::VulkanBackendContext backendContext;
        backendContext.fInstance = instance_;
        backendContext.fPhysicalDevice = physicalDevice_;
        backendContext.fDevice = device_;
        backendContext.fQueue = queue_;
        backendContext.fGraphicsQueueIndex = queueFamilyIndex_;
        backendContext.fMaxAPIVersion = VK_API_VERSION_1_1;
        backendContext.fGetProc = [](const char* name, VkInstance instance, VkDevice device) {
            if (device != VK_NULL_HANDLE) return vkGetDeviceProcAddr(device, name);
            return vkGetInstanceProcAddr(instance, name);
        };
        const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        vulkanExtensions_.init(
            backendContext.fGetProc,
            instance_,
            physicalDevice_,
            2,
            instanceExtensions,
            static_cast<uint32_t>(deviceExtensions.size()),
            deviceExtensions.data());
        backendContext.fVkExtensions = &vulkanExtensions_;
        backendContext.fMemoryAllocator = SkiaVMA::Make(backendContext, SkiaVMA::Options{});
        if (!backendContext.fMemoryAllocator) {
            logError("SkiaVMA could not create a Vulkan memory allocator");
            return false;
        }
        context_ = skgpu::graphite::ContextFactory::MakeVulkan(
            backendContext,
            skgpu::graphite::ContextOptions{});
        if (context_ == nullptr) {
            logError("Skia Graphite could not create a Vulkan context");
            return false;
        }
        srgbColorSpace_ = SkColorSpace::MakeSRGB();
        frameSlots_.reserve(kFramesInFlight);
        for (size_t index = 0; index < kFramesInFlight; ++index) {
            FrameSlot frameSlot;
            frameSlot.recorder = context_->makeRecorder();
            if (frameSlot.recorder == nullptr) {
                logError("Skia Graphite could not create a frame recorder");
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!succeeded(
                    vkCreateSemaphore(
                        device_,
                        &semaphoreInfo,
                        nullptr,
                        &frameSlot.imageAvailableSemaphore),
                    "vkCreateSemaphore(image available)")) {
                return false;
            }
            if (!succeeded(
                    vkCreateSemaphore(
                        device_,
                        &semaphoreInfo,
                        nullptr,
                        &frameSlot.completionSemaphore),
                    "vkCreateSemaphore(completion)")) {
                return false;
            }
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkExportFenceCreateInfo exportFenceInfo{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
            if (hardwareBufferOutput_) {
                exportFenceInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
                fenceInfo.pNext = &exportFenceInfo;
            }
            if (!succeeded(
                    vkCreateFence(device_, &fenceInfo, nullptr, &frameSlot.completionFence),
                    "vkCreateFence(completion)")) {
                return false;
            }
            frameSlots_.push_back(std::move(frameSlot));
        }
        return true;
    }

    bool createSwapchain(int requestedWidth, int requestedHeight) {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                    physicalDevice_,
                    surface_,
                    &capabilities),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
            return false;
        }

        uint32_t formatCount = 0;
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceFormatsKHR(
                    physicalDevice_,
                    surface_,
                    &formatCount,
                    nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
            return false;
        }
        if (formatCount == 0) {
            logError("Vulkan surface exposes no swapchain format");
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceFormatsKHR(
                    physicalDevice_,
                    surface_,
                    &formatCount,
                    formats.data()),
                "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
            return false;
        }
        VkSurfaceFormatKHR surfaceFormat = formats.front();
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
                candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
                surfaceFormat = candidate;
                break;
            }
        }
        swapchainFormat_ = surfaceFormat.format;
        colorSpace_ = surfaceFormat.colorSpace;

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != UINT32_MAX) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(
                requestedWidth,
                static_cast<int>(capabilities.minImageExtent.width),
                static_cast<int>(capabilities.maxImageExtent.width));
            extent.height = std::clamp(
                requestedHeight,
                static_cast<int>(capabilities.minImageExtent.height),
                static_cast<int>(capabilities.maxImageExtent.height));
        }
        if (extent.width == 0 || extent.height == 0) {
            logError("Vulkan surface has zero-sized swapchain extent");
            return false;
        }
        swapchainExtent_ = extent;
        surfaceWidth_ = static_cast<int>(extent.width);
        surfaceHeight_ = static_cast<int>(extent.height);

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }
        swapchainImageUsage_ = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT;
        if ((capabilities.supportedUsageFlags & swapchainImageUsage_) != swapchainImageUsage_) {
            logError("Vulkan surface does not support color-attachment swapchain images");
            return false;
        }

        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
            const VkCompositeAlphaFlagBitsKHR candidates[] = {
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            };
            for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
                if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
                    compositeAlpha = candidate;
                    break;
                }
            }
        }

        VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surface_;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = swapchainFormat_;
        swapchainInfo.imageColorSpace = colorSpace_;
        swapchainInfo.imageExtent = swapchainExtent_;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = swapchainImageUsage_;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha = compositeAlpha;
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;
        if (!succeeded(
                vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_),
                "vkCreateSwapchainKHR")) {
            return false;
        }

        uint32_t swapchainImageCount = 0;
        if (!succeeded(
                vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, nullptr),
                "vkGetSwapchainImagesKHR")) {
            return false;
        }
        swapchainImages_.resize(swapchainImageCount);
        if (!succeeded(
                vkGetSwapchainImagesKHR(
                    device_,
                    swapchain_,
                    &swapchainImageCount,
                    swapchainImages_.data()),
                "vkGetSwapchainImagesKHR")) {
            return false;
        }
        imageLayouts_.assign(swapchainImages_.size(), VK_IMAGE_LAYOUT_UNDEFINED);

        renderFinishedSemaphores_.resize(swapchainImageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& semaphore : renderFinishedSemaphores_) {
            if (!succeeded(
                    vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore),
                    "vkCreateSemaphore(render finished)")) {
                return false;
            }
        }
        imageInFlightFrameSlots_.assign(swapchainImageCount, -1);
        swapchainOutOfDate_ = false;
        return true;
    }

    void destroySwapchain() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        resetActiveFrame();
        if (context_ != nullptr) {
            context_->freeGpuResources();
        }
        for (VkSemaphore semaphore : renderFinishedSemaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        renderFinishedSemaphores_.clear();
        imageInFlightFrameSlots_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapchainImages_.clear();
        imageLayouts_.clear();
    }

    void clearSurface() {
        destroyHardwareBufferOutput();
        destroySwapchain();
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    bool useHardwareBuffer_ = false;
    bool hardwareBufferOutput_ = false;
    ASurfaceControl* hardwareSurfaceControl_ = nullptr;
    bool hardwareSurfaceGeometrySet_ = false;
    std::shared_ptr<HardwareBufferState> lastHardwareBufferState_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;

    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkImageUsageFlags swapchainImageUsage_ = 0;
    VkExtent2D swapchainExtent_{};
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageLayout> imageLayouts_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<int> imageInFlightFrameSlots_;
    uint32_t currentImageIndex_ = 0;
    std::vector<FrameSlot> frameSlots_;
    size_t nextFrameSlotIndex_ = 0;
    int activeFrameSlotIndex_ = -1;
    bool swapchainOutOfDate_ = false;
    uint64_t nextFrameId_ = 1;
    int64_t frameTimeNanos_ = 0;

    skgpu::VulkanExtensions vulkanExtensions_;
    std::unique_ptr<skgpu::graphite::Context> context_;
    sk_sp<SkColorSpace> srgbColorSpace_;
    sk_sp<SkSurface> frameSurface_;
    SkCanvas* canvas_ = nullptr;
    SkPathBuilder pathBuilder_;
};

GraphiteEngine* fromHandle(jlong handle) {
    return reinterpret_cast<GraphiteEngine*>(static_cast<uintptr_t>(handle));
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_create(
    JNIEnv*,
    jclass,
    jboolean useHardwareBuffer) {
    auto* engine = new GraphiteEngine(useHardwareBuffer == JNI_TRUE);
    if (!engine->initialize()) {
        delete engine;
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(engine));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_setSurface(
    JNIEnv* env,
    jclass,
    jlong handle,
    jobject surface,
    jint width,
    jint height) {
    GraphiteEngine* engine = fromHandle(handle);
    return engine != nullptr && engine->setSurface(env, surface, width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_beginFrame(
    JNIEnv*,
    jclass,
    jlong handle) {
    GraphiteEngine* engine = fromHandle(handle);
    return engine != nullptr && engine->beginFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_setFrameTimeNanos(
    JNIEnv*,
    jclass,
    jlong handle,
    jlong frameTimeNanos) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->setFrameTimeNanos(static_cast<int64_t>(frameTimeNanos));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_endFrame(
    JNIEnv*,
    jclass,
    jlong handle) {
    GraphiteEngine* engine = fromHandle(handle);
    return engine != nullptr && engine->endFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_dispose(
    JNIEnv*,
    jclass,
    jlong handle) {
    GraphiteEngine* engine = fromHandle(handle);
    if (engine != nullptr) {
        delete engine;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_clear(
    JNIEnv*,
    jclass,
    jlong handle,
    jint color) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->clear(static_cast<uint32_t>(color));
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_save(JNIEnv*, jclass, jlong handle) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->save();
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_restore(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->restore();
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_translate(
    JNIEnv*,
    jclass,
    jlong handle,
    jfloat x,
    jfloat y) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->translate(x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_rotate(
    JNIEnv*,
    jclass,
    jlong handle,
    jfloat degrees) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->rotate(degrees);
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_concat(
    JNIEnv* env,
    jclass,
    jlong handle,
    jfloatArray columnMajor) {
    if (columnMajor == nullptr || env->GetArrayLength(columnMajor) != 16) return;
    std::array<float, 16> values{};
    env->GetFloatArrayRegion(columnMajor, 0, 16, values.data());
    if (GraphiteEngine* engine = fromHandle(handle)) engine->concat(values.data());
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_clipRect(
    JNIEnv*, jclass, jlong handle, jfloat left, jfloat top, jfloat right, jfloat bottom,
    jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->clipRect(left, top, right, bottom, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_beginPath(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->beginPath();
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_moveTo(
    JNIEnv*,
    jclass,
    jlong handle,
    jfloat x,
    jfloat y) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->moveTo(x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_lineTo(
    JNIEnv*,
    jclass,
    jlong handle,
    jfloat x,
    jfloat y) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->lineTo(x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_closePath(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (GraphiteEngine* engine = fromHandle(handle)) engine->closePath();
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawPath(
    JNIEnv*,
    jclass,
    jlong handle,
    jint color,
    jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawPath(static_cast<uint32_t>(color), antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawImmutablePath(
    JNIEnv* env, jclass, jlong handle, jbyteArray verbs, jfloatArray points, jint color,
    jboolean stroke, jfloat strokeWidth, jboolean antiAlias) {
    if (verbs == nullptr || points == nullptr) return;
    std::vector<uint8_t> nativeVerbs(static_cast<size_t>(env->GetArrayLength(verbs)));
    std::vector<float> nativePoints(static_cast<size_t>(env->GetArrayLength(points)));
    env->GetByteArrayRegion(
            verbs, 0, static_cast<jsize>(nativeVerbs.size()),
            reinterpret_cast<jbyte*>(nativeVerbs.data()));
    env->GetFloatArrayRegion(points, 0, static_cast<jsize>(nativePoints.size()), nativePoints.data());
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawImmutablePath(
                nativeVerbs, nativePoints, static_cast<uint32_t>(color), stroke == JNI_TRUE,
                strokeWidth, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawRect(
    JNIEnv*, jclass, jlong handle, jfloat left, jfloat top, jfloat right, jfloat bottom,
    jint color, jboolean stroke, jfloat strokeWidth, jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawRect(left, top, right, bottom, static_cast<uint32_t>(color),
                         stroke == JNI_TRUE, strokeWidth, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawRoundRect(
    JNIEnv*, jclass, jlong handle, jfloat left, jfloat top, jfloat right, jfloat bottom,
    jfloat radiusX, jfloat radiusY, jint color, jboolean stroke, jfloat strokeWidth,
    jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawRoundRect(left, top, right, bottom, radiusX, radiusY,
                              static_cast<uint32_t>(color), stroke == JNI_TRUE,
                              strokeWidth, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawOval(
    JNIEnv*, jclass, jlong handle, jfloat left, jfloat top, jfloat right, jfloat bottom,
    jint color, jboolean stroke, jfloat strokeWidth, jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawOval(left, top, right, bottom, static_cast<uint32_t>(color),
                         stroke == JNI_TRUE, strokeWidth, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawCircle(
    JNIEnv*, jclass, jlong handle, jfloat x, jfloat y, jfloat radius, jint color,
    jboolean stroke, jfloat strokeWidth, jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawCircle(x, y, radius, static_cast<uint32_t>(color), stroke == JNI_TRUE,
                           strokeWidth, antiAlias == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_AndroidGraphiteNative_drawLine(
    JNIEnv*, jclass, jlong handle, jfloat x0, jfloat y0, jfloat x1, jfloat y1,
    jint color, jfloat strokeWidth, jboolean antiAlias) {
    if (GraphiteEngine* engine = fromHandle(handle)) {
        engine->drawLine(x0, y0, x1, y1, static_cast<uint32_t>(color), strokeWidth,
                         antiAlias == JNI_TRUE);
    }
}
