#include <jni.h>
#include <android/log.h>
#include <libuvc/libuvc.h>
#include <libusb.h>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ThermalEdgeNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ThermalEdgeNative", __VA_ARGS__)

static JavaVM* gVm = nullptr;
static uvc_context_t* gCtx = nullptr;
static uvc_device_handle_t* gDevh = nullptr;
static jobject gCallback = nullptr;
static jmethodID gOnFrame = nullptr;
static int gWrappedFd = -1;
static std::atomic<bool> gStreaming(false);
static std::mutex gMutex;

static void releaseCallback(JNIEnv* env) {
    if (gCallback) {
        env->DeleteGlobalRef(gCallback);
        gCallback = nullptr;
        gOnFrame = nullptr;
    }
}

static void frameCallback(uvc_frame_t* frame, void*) {
    if (!gStreaming.load() || !frame || !frame->data || frame->width < 3 || frame->height < 3) return;
    if (frame->frame_format != UVC_FRAME_FORMAT_YUYV && frame->frame_format != UVC_FRAME_FORMAT_UYVY) return;

    const int width = static_cast<int>(frame->width);
    const int imageHeight = static_cast<int>(frame->height > 192 ? 192 : frame->height);
    std::vector<jbyte> luma(width * imageHeight);
    const auto* data = static_cast<const uint8_t*>(frame->data);
    const size_t step = frame->step > 0 ? frame->step : static_cast<size_t>(width * 2);

    for (int y = 0; y < imageHeight; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 0; x < width; ++x) {
            const int byteIndex = x * 2;
            uint8_t value = frame->frame_format == UVC_FRAME_FORMAT_YUYV ? row[byteIndex] : row[byteIndex + 1];
            luma[y * width + x] = static_cast<jbyte>(value);
        }
    }

    JNIEnv* env = nullptr;
    bool detach = false;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        detach = true;
    }

    jobject callbackObj = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard<std::mutex> guard(gMutex);
        callbackObj = gCallback;
        method = gOnFrame;
    }

    if (callbackObj && method) {
        jbyteArray arr = env->NewByteArray(static_cast<jsize>(luma.size()));
        if (arr) {
            env->SetByteArrayRegion(arr, 0, static_cast<jsize>(luma.size()), luma.data());
            env->CallVoidMethod(callbackObj, method, arr, width, imageHeight);
            env->DeleteLocalRef(arr);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Java frame callback threw an exception");
            }
        }
    }

    if (detach) gVm->DetachCurrentThread();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeOpen(JNIEnv* env, jobject, jint fd) {
    std::lock_guard<std::mutex> guard(gMutex);
    if (gDevh) return JNI_TRUE;

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    uvc_error_t initResult = uvc_init(&gCtx, nullptr);
    if (initResult != UVC_SUCCESS) {
        LOGE("uvc_init failed: %d", initResult);
        gCtx = nullptr;
        return JNI_FALSE;
    }

    gWrappedFd = dup(fd);
    if (gWrappedFd < 0) {
        LOGE("dup(fd) failed");
        uvc_exit(gCtx);
        gCtx = nullptr;
        return JNI_FALSE;
    }

    uvc_error_t openResult = uvc_wrap(gWrappedFd, gCtx, &gDevh);
    if (openResult != UVC_SUCCESS || !gDevh) {
        LOGE("uvc_wrap failed: %d", openResult);
        close(gWrappedFd);
        gWrappedFd = -1;
        uvc_exit(gCtx);
        gCtx = nullptr;
        gDevh = nullptr;
        return JNI_FALSE;
    }

    LOGI("UVC camera opened");
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeStartStream(JNIEnv* env, jobject, jobject callback) {
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gDevh || gStreaming.load()) return gStreaming.load() ? JNI_TRUE : JNI_FALSE;

    releaseCallback(env);
    gCallback = env->NewGlobalRef(callback);
    jclass cls = env->GetObjectClass(callback);
    gOnFrame = env->GetMethodID(cls, "onFrame", "([BII)V");
    env->DeleteLocalRef(cls);
    if (!gCallback || !gOnFrame) {
        releaseCallback(env);
        return JNI_FALSE;
    }

    uvc_stream_ctrl_t ctrl{};
    uvc_error_t result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
    if (result != UVC_SUCCESS) {
        LOGI("256x196@25 unavailable, trying 256x192@25");
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
    }
    if (result != UVC_SUCCESS) {
        LOGE("Failed to negotiate T2 Pro YUYV stream: %d", result);
        releaseCallback(env);
        return JNI_FALSE;
    }

    gStreaming.store(true);
    result = uvc_start_streaming(gDevh, &ctrl, frameCallback, nullptr, 0);
    if (result != UVC_SUCCESS) {
        gStreaming.store(false);
        LOGE("uvc_start_streaming failed: %d", result);
        releaseCallback(env);
        return JNI_FALSE;
    }
    LOGI("Thermal stream started");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeStopStream(JNIEnv* env, jobject) {
    bool wasStreaming = gStreaming.exchange(false);
    if (wasStreaming && gDevh) uvc_stop_streaming(gDevh);
    std::lock_guard<std::mutex> guard(gMutex);
    releaseCallback(env);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeClose(JNIEnv* env, jobject) {
    if (gStreaming.exchange(false) && gDevh) uvc_stop_streaming(gDevh);
    std::lock_guard<std::mutex> guard(gMutex);
    releaseCallback(env);
    if (gDevh) {
        uvc_close(gDevh);
        gDevh = nullptr;
    }
    if (gWrappedFd >= 0) {
        close(gWrappedFd);
        gWrappedFd = -1;
    }
    if (gCtx) {
        uvc_exit(gCtx);
        gCtx = nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeIsStreaming(JNIEnv*, jobject) {
    return gStreaming.load() ? JNI_TRUE : JNI_FALSE;
}
