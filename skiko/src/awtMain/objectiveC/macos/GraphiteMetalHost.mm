#ifdef SK_METAL

#import <jawt.h>
#import <jawt_md.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

@interface GraphiteMetalDevice : NSObject
@property(nonatomic, weak) CALayer *container;
@property(nonatomic, strong) CAMetalLayer *layer;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<CAMetalDrawable> drawable;
@end

@implementation GraphiteMetalDevice
@end

static GraphiteMetalDevice *deviceFromHandle(jlong handle) {
    return (__bridge GraphiteMetalDevice *)(void *)handle;
}

extern "C" {

JNIEXPORT jlong JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeCreate(
    JNIEnv *env, jobject host, jlong platformInfo, jfloat scale)
{
    @autoreleasepool {
        if (platformInfo == 0) return 0;

        NSObject<JAWT_SurfaceLayers> *surfaceLayers =
            (__bridge NSObject<JAWT_SurfaceLayers> *)(void *)platformInfo;
        CALayer *container = [surfaceLayers windowLayer];
        id<MTLDevice> metalDevice = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> commandQueue = [metalDevice newCommandQueue];
        if (container == nil || metalDevice == nil || commandQueue == nil) return 0;

        GraphiteMetalDevice *device = [GraphiteMetalDevice new];
        CAMetalLayer *layer = [CAMetalLayer layer];
        CGFloat contentsScale = scale > 0.0f ? scale : 1.0f;

        layer.device = metalDevice;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.contentsScale = contentsScale;
        layer.contentsGravity = kCAGravityTopLeft;
        layer.framebufferOnly = NO;
        layer.opaque = YES;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        layer.needsDisplayOnBoundsChange = YES;
        layer.frame = container.bounds;
        layer.drawableSize = CGSizeMake(
            container.bounds.size.width * contentsScale,
            container.bounds.size.height * contentsScale
        );

        [container addSublayer:layer];
        device.container = container;
        device.layer = layer;
        device.device = metalDevice;
        device.queue = commandQueue;
        return (jlong)(__bridge_retained void *)device;
    }
}

JNIEXPORT void JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeResize(
    JNIEnv *env, jobject host, jlong handle, jint x, jint y, jint width, jint height, jfloat scale)
{
    @autoreleasepool {
        GraphiteMetalDevice *device = deviceFromHandle(handle);
        if (device == nil || device.layer == nil) return;

        CGFloat contentsScale = scale > 0.0f ? scale : 1.0f;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        device.layer.contentsScale = contentsScale;
        // The CAMetalLayer is attached to the AWT surface layer, so its frame
        // must be local to that layer rather than offset by the parent window.
        device.layer.frame = CGRectMake(0, 0, width, height);
        if (width > 0 && height > 0) {
            device.layer.drawableSize = CGSizeMake(
                width * contentsScale,
                height * contentsScale
            );
        }
        [CATransaction commit];
        [CATransaction flush];
    }
}

JNIEXPORT jlong JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeNextDrawable(
    JNIEnv *env, jobject host, jlong handle)
{
    @autoreleasepool {
        GraphiteMetalDevice *device = deviceFromHandle(handle);
        if (device == nil || device.layer == nil) return 0;

        device.drawable = nil;
        device.drawable = [device.layer nextDrawable];
        if (device.drawable == nil || device.drawable.texture == nil) return 0;
        return (jlong)(__bridge void *)device.drawable.texture;
    }
}

JNIEXPORT jlong JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeDevice(
    JNIEnv *env, jobject host, jlong handle)
{
    GraphiteMetalDevice *device = deviceFromHandle(handle);
    return device == nil ? 0 : (jlong)(__bridge void *)device.device;
}

JNIEXPORT jlong JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeQueue(
    JNIEnv *env, jobject host, jlong handle)
{
    GraphiteMetalDevice *device = deviceFromHandle(handle);
    return device == nil ? 0 : (jlong)(__bridge void *)device.queue;
}

JNIEXPORT void JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativePresent(
    JNIEnv *env, jobject host, jlong handle)
{
    @autoreleasepool {
        GraphiteMetalDevice *device = deviceFromHandle(handle);
        if (device == nil || device.drawable == nil || device.queue == nil) return;

        id<MTLCommandBuffer> commandBuffer = [device.queue commandBuffer];
        if (commandBuffer != nil) {
            [commandBuffer presentDrawable:device.drawable];
            [commandBuffer commit];
        }
        device.drawable = nil;
    }
}

JNIEXPORT void JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeDropDrawable(
    JNIEnv *env, jobject host, jlong handle)
{
    @autoreleasepool {
        GraphiteMetalDevice *device = deviceFromHandle(handle);
        if (device != nil) device.drawable = nil;
    }
}

JNIEXPORT void JNICALL Java_org_jetbrains_skiko_graphite_GraphiteMetalHost_nativeDispose(
    JNIEnv *env, jobject host, jlong handle)
{
    @autoreleasepool {
        GraphiteMetalDevice *device = (__bridge_transfer GraphiteMetalDevice *)(void *)handle;
        if (device == nil) return;
        device.drawable = nil;
        [device.layer removeFromSuperlayer];
        device.layer = nil;
        device.queue = nil;
        device.device = nil;
    }
}

}

#endif
