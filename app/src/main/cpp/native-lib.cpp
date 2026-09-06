#include <jni.h>
#include <android/log.h>
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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
static int gVendorId = 0;
static int gProductId = 0;
static std::atomic<bool> gStreaming(false);
static std::mutex gMutex;
static float gWindowLow = -1.0f;
static float gWindowHigh = -1.0f;

static void releaseCallback(JNIEnv* env) {
    if (gCallback) {
        env->DeleteGlobalRef(gCallback);
        gCallback = nullptr;
        gOnFrame = nullptr;
    }
}

static uint16_t read16(const uint8_t* p, bool bigEndian) {
    return bigEndian
            ? static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1])
            : static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static bool chooseBigEndian(const uint8_t* data, size_t step, int width, int height) {
    // A thermal scene is spatially smooth. Pick the byte order whose neighboring 16-bit
    // samples change less. This keeps the decoder useful across T2 Pro firmware revisions.
    uint64_t leScore = 0;
    uint64_t beScore = 0;
    int samples = 0;
    for (int y = 4; y < height - 4; y += 8) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 8; x < width - 8; x += 8) {
            const uint8_t* a = row + static_cast<size_t>(x - 1) * 2;
            const uint8_t* b = row + static_cast<size_t>(x) * 2;
            leScore += static_cast<uint64_t>(std::abs(static_cast<int>(read16(a, false)) - static_cast<int>(read16(b, false))));
            beScore += static_cast<uint64_t>(std::abs(static_cast<int>(read16(a, true)) - static_cast<int>(read16(b, true))));
            samples++;
        }
    }
    return samples > 0 && beScore < leScore;
}

static void decodeRaw16(const uint8_t* data, size_t step, int width, int height, std::vector<jbyte>& out) {
    const int count = width * height;
    std::vector<uint16_t> raw(static_cast<size_t>(count));
    std::array<int, 4096> hist{};
    bool bigEndian = chooseBigEndian(data, step, width, height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 0; x < width; ++x) {
            uint16_t value = read16(row + static_cast<size_t>(x) * 2, bigEndian);
            raw[static_cast<size_t>(y) * width + x] = value;
            hist[value >> 4]++;
        }
    }

    // Ignore the hottest/coldest 1% when stretching the scene. A single bad detector pixel
    // or telemetry-looking value otherwise destroys the contrast of the whole thermal frame.
    const int lowTarget = std::max(1, count / 100);
    const int highTarget = std::max(lowTarget + 1, count - count / 100);
    int cumulative = 0;
    int lowBin = 0;
    int highBin = 4095;
    for (int i = 0; i < 4096; ++i) {
        cumulative += hist[i];
        if (cumulative >= lowTarget) {
            lowBin = i;
            break;
        }
    }
    cumulative = 0;
    for (int i = 0; i < 4096; ++i) {
        cumulative += hist[i];
        if (cumulative >= highTarget) {
            highBin = i;
            break;
        }
    }

    float low = static_cast<float>(lowBin << 4);
    float high = static_cast<float>(((highBin + 1) << 4) - 1);
    if (high - low < 96.0f) {
        float mid = (high + low) * 0.5f;
        low = mid - 48.0f;
        high = mid + 48.0f;
    }

    if (gWindowLow < 0.0f || gWindowHigh < 0.0f) {
        gWindowLow = low;
        gWindowHigh = high;
    } else {
        constexpr float alpha = 0.18f;
        gWindowLow += (low - gWindowLow) * alpha;
        gWindowHigh += (high - gWindowHigh) * alpha;
    }

    const float denom = std::max(32.0f, gWindowHigh - gWindowLow);
    for (int i = 0; i < count; ++i) {
        float scaled = (static_cast<float>(raw[static_cast<size_t>(i)]) - gWindowLow) * 255.0f / denom;
        int value = static_cast<int>(scaled + 0.5f);
        value = std::max(0, std::min(255, value));
        out[static_cast<size_t>(i)] = static_cast<jbyte>(static_cast<uint8_t>(value));
    }

    static int logCounter = 0;
    if ((logCounter++ % 100) == 0) {
        LOGI("RAW16 decode endian=%s window=%.1f..%.1f VID=%04x PID=%04x",
             bigEndian ? "BE" : "LE", gWindowLow, gWindowHigh, gVendorId, gProductId);
    }
}

static void decodeYuvLuma(const uint8_t* data, size_t step, int width, int height,
                          enum uvc_frame_format format, std::vector<jbyte>& out) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 0; x < width; ++x) {
            const int byteIndex = x * 2;
            uint8_t value = format == UVC_FRAME_FORMAT_YUYV ? row[byteIndex] : row[byteIndex + 1];
            out[static_cast<size_t>(y) * width + x] = static_cast<jbyte>(value);
        }
    }
}

static void frameCallback(uvc_frame_t* frame, void*) {
    if (!gStreaming.load() || !frame || !frame->data || frame->width < 3 || frame->height < 3) return;
    if (frame->frame_format != UVC_FRAME_FORMAT_YUYV && frame->frame_format != UVC_FRAME_FORMAT_UYVY) return;

    const int width = static_cast<int>(frame->width);
    const int imageHeight = static_cast<int>(frame->height > 192 ? 192 : frame->height);
    std::vector<jbyte> luma(static_cast<size_t>(width) * imageHeight);
    const auto* data = static_cast<const uint8_t*>(frame->data);
    const size_t step = frame->step > 0 ? frame->step : static_cast<size_t>(width * 2);

    // T2 Pro_A2 exposes 256x196 as YUYV even though the first 192 lines are effectively
    // 16-bit thermal samples. Treating only the Y byte as luminance creates the green noise
    // block seen in v0.1. 256x192, when offered by firmware, is treated as normalized YUV.
    const bool raw16 = (width == 256 && frame->height >= 196) ||
                       (gVendorId == 0x3474 && gProductId == 0x6002 && frame->height > 192);
    if (raw16) {
        decodeRaw16(data, step, width, imageHeight, luma);
    } else {
        decodeYuvLuma(data, step, width, imageHeight, frame->frame_format, luma);
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
Java_com_ikegami99_thermaledge_ThermalCamera_nativeOpen(JNIEnv*, jobject, jint fd, jint vendorId, jint productId) {
    std::lock_guard<std::mutex> guard(gMutex);
    if (gDevh) return JNI_TRUE;

    gVendorId = vendorId;
    gProductId = productId;
    gWindowLow = -1.0f;
    gWindowHigh = -1.0f;

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

    LOGI("UVC camera opened VID=%04x PID=%04x", gVendorId, gProductId);
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
    // Prefer a normalized 256x192 stream if the firmware exposes one. T2 Pro_A2 commonly
    // exposes only 256x196, which is handled as RAW16 in frameCallback.
    uvc_error_t result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
    if (result != UVC_SUCCESS) {
        LOGI("256x192@25 unavailable, trying T2 Pro raw 256x196@25");
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
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
    gVendorId = 0;
    gProductId = 0;
    gWindowLow = -1.0f;
    gWindowHigh = -1.0f;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeIsStreaming(JNIEnv*, jobject) {
    return gStreaming.load() ? JNI_TRUE : JNI_FALSE;
}
