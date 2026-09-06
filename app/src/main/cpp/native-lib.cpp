#include <jni.h>
#include <android/log.h>
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
static std::vector<uint16_t> gPreviousRaw;

static std::atomic<int> gRawModeResult(9999);
static std::atomic<int> gCalibrateResult(9999);
static std::atomic<int> gZoomReadResult(9999);
static std::atomic<int> gZoomReadback(-1);
static std::atomic<int> gFrameWidth(0);
static std::atomic<int> gFrameHeight(0);
static std::atomic<int> gFrameFormat(0);
static std::atomic<int> gFrameStep(0);
static std::atomic<int> gRawMin(0);
static std::atomic<int> gRawMax(0);
static std::atomic<int> gRawMean(0);
static std::atomic<int> gRawDeltaX100(0);
static std::atomic<int> gWindowLowInt(0);
static std::atomic<int> gWindowHighInt(0);
static std::atomic<long long> gFrameCounter(0);
static std::atomic<int> gDecoderMode(0); // 0 unknown, 1 YUV luma, 2 RAW16 LE

static bool isT2Pro() {
    return gVendorId == 0x3474 && gProductId == 0x6002;
}

static void resetDiagnostics() {
    gRawModeResult.store(9999);
    gCalibrateResult.store(9999);
    gZoomReadResult.store(9999);
    gZoomReadback.store(-1);
    gFrameWidth.store(0);
    gFrameHeight.store(0);
    gFrameFormat.store(0);
    gFrameStep.store(0);
    gRawMin.store(0);
    gRawMax.store(0);
    gRawMean.store(0);
    gRawDeltaX100.store(0);
    gWindowLowInt.store(0);
    gWindowHighInt.store(0);
    gFrameCounter.store(0);
    gDecoderMode.store(0);
    gWindowLow = -1.0f;
    gWindowHigh = -1.0f;
    gPreviousRaw.clear();
}

static void releaseCallback(JNIEnv* env) {
    if (gCallback) {
        env->DeleteGlobalRef(gCallback);
        gCallback = nullptr;
        gOnFrame = nullptr;
    }
}

static uint16_t read16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static void decodeRaw16(const uint8_t* data, size_t step, int width, int height,
                        std::vector<jbyte>& out) {
    const int count = width * height;
    std::vector<uint16_t> raw(static_cast<size_t>(count));
    std::array<int, 4096> hist{};

    uint16_t rawMin = 0xffff;
    uint16_t rawMax = 0;
    uint64_t rawSum = 0;
    uint64_t deltaSum = 0;
    const bool havePrevious = gPreviousRaw.size() == raw.size();

    // InfiRay/Xinfrared 256x196 streams are exposed through UVC as YUYV, but in RAW mode
    // each two-byte pixel is a native little-endian uint16 sample. The final four rows are
    // camera metadata; the caller passes only the 192 image rows here.
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            uint16_t value = read16le(row + static_cast<size_t>(x) * 2);
            raw[i] = value;
            rawMin = std::min(rawMin, value);
            rawMax = std::max(rawMax, value);
            rawSum += value;
            if (havePrevious) {
                deltaSum += static_cast<uint64_t>(std::abs(static_cast<int>(value) - static_cast<int>(gPreviousRaw[i])));
            }
            hist[value >> 4]++;
        }
    }

    gRawMin.store(rawMin);
    gRawMax.store(rawMax);
    gRawMean.store(count > 0 ? static_cast<int>(rawSum / static_cast<uint64_t>(count)) : 0);
    gRawDeltaX100.store(havePrevious && count > 0
                        ? static_cast<int>((deltaSum * 100ULL) / static_cast<uint64_t>(count))
                        : 0);
    gPreviousRaw = raw;

    // Percentile AGC. Ignore the outer 1% so metadata-like outliers or bad pixels do not
    // flatten the useful thermal contrast.
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
    if (high - low < 64.0f) {
        float mid = (high + low) * 0.5f;
        low = mid - 32.0f;
        high = mid + 32.0f;
    }

    if (gWindowLow < 0.0f || gWindowHigh < 0.0f) {
        gWindowLow = low;
        gWindowHigh = high;
    } else {
        constexpr float alpha = 0.20f;
        gWindowLow += (low - gWindowLow) * alpha;
        gWindowHigh += (high - gWindowHigh) * alpha;
    }
    gWindowLowInt.store(static_cast<int>(gWindowLow));
    gWindowHighInt.store(static_cast<int>(gWindowHigh));

    const float denom = std::max(24.0f, gWindowHigh - gWindowLow);
    for (int i = 0; i < count; ++i) {
        float scaled = (static_cast<float>(raw[static_cast<size_t>(i)]) - gWindowLow) * 255.0f / denom;
        int value = static_cast<int>(scaled + 0.5f);
        value = std::max(0, std::min(255, value));
        out[static_cast<size_t>(i)] = static_cast<jbyte>(static_cast<uint8_t>(value));
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
    const int physicalHeight = static_cast<int>(frame->height);
    const int imageHeight = physicalHeight > 192 ? 192 : physicalHeight;
    const auto* data = static_cast<const uint8_t*>(frame->data);
    const size_t step = frame->step > 0 ? frame->step : static_cast<size_t>(width * 2);
    std::vector<jbyte> luma(static_cast<size_t>(width) * imageHeight);

    gFrameWidth.store(width);
    gFrameHeight.store(physicalHeight);
    gFrameFormat.store(static_cast<int>(frame->frame_format));
    gFrameStep.store(static_cast<int>(step));
    gFrameCounter.fetch_add(1);

    const bool raw16 = (width == 256 && physicalHeight >= 196) && isT2Pro();
    if (raw16) {
        gDecoderMode.store(2);
        decodeRaw16(data, step, width, imageHeight, luma);
    } else {
        gDecoderMode.store(1);
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
    resetDiagnostics();

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

    if (isT2Pro()) {
        // InfiRay's PC/open-source tools use the UVC absolute-zoom control as a command bus.
        // 0x8004 requests 16-bit raw thermal output instead of the misleading YUYV-looking
        // default frame that produces a stationary green fixed-pattern texture.
        uvc_error_t rawResult = uvc_set_zoom_abs(gDevh, 0x8004);
        gRawModeResult.store(static_cast<int>(rawResult));
        uint16_t zoom = 0;
        uvc_error_t readResult = uvc_get_zoom_abs(gDevh, &zoom, UVC_GET_CUR);
        gZoomReadResult.store(static_cast<int>(readResult));
        gZoomReadback.store(static_cast<int>(zoom));
        LOGI("T2 Pro raw-mode command 0x8004 result=%d read=%d current=0x%04x",
             rawResult, readResult, zoom);
        usleep(120000);
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
    uvc_error_t result;
    if (isT2Pro()) {
        // T2Pro_A2 advertises 256x196 YUYV. In raw mode the first 192 rows are uint16
        // thermal samples and the final four rows carry camera metadata.
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
        if (result != UVC_SUCCESS) {
            LOGI("T2 Pro 256x196 unavailable, trying 256x192 fallback");
            result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
        }
    } else {
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
        if (result != UVC_SUCCESS) {
            result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
        }
    }

    if (result != UVC_SUCCESS) {
        LOGE("Failed to negotiate thermal stream: %d", result);
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

    if (isT2Pro()) {
        // Let RAW mode settle for a few frames, then trigger the camera's shutter/NUC command.
        // Open-source InfiRay tools issue 0x8000 for this calibration step.
        usleep(300000);
        uvc_error_t calResult = uvc_set_zoom_abs(gDevh, 0x8000);
        gCalibrateResult.store(static_cast<int>(calResult));
        LOGI("T2 Pro NUC/calibration command 0x8000 result=%d", calResult);
        usleep(180000);
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
    resetDiagnostics();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeIsStreaming(JNIEnv*, jobject) {
    return gStreaming.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeGetDiagnostics(JNIEnv* env, jobject) {
    char buffer[768];
    const int mode = gDecoderMode.load();
    const char* modeName = mode == 2 ? "RAW16_LE" : (mode == 1 ? "YUV_LUMA" : "UNKNOWN");
    std::snprintf(buffer, sizeof(buffer),
                  "VID=%04X PID=%04X rawCmd=%d nucCmd=%d zoomRead=%d zoom=0x%04X "
                  "frame=%dx%d fmt=%d step=%d decoder=%s count=%lld "
                  "raw[min=%d max=%d mean=%d delta=%.2f] window=%d..%d",
                  gVendorId, gProductId,
                  gRawModeResult.load(), gCalibrateResult.load(),
                  gZoomReadResult.load(), gZoomReadback.load() & 0xffff,
                  gFrameWidth.load(), gFrameHeight.load(), gFrameFormat.load(), gFrameStep.load(),
                  modeName, static_cast<long long>(gFrameCounter.load()),
                  gRawMin.load(), gRawMax.load(), gRawMean.load(),
                  gRawDeltaX100.load() / 100.0,
                  gWindowLowInt.load(), gWindowHighInt.load());
    return env->NewStringUTF(buffer);
}
