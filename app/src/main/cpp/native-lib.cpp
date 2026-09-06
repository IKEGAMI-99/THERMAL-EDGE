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
#include <thread>
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
static std::mutex gFpnMutex;

static float gWindowLow = -1.0f;
static float gWindowHigh = -1.0f;
static std::vector<uint16_t> gPreviousRaw;
static std::vector<float> gFpnReference;
static std::vector<double> gFpnAccum;

static std::atomic<int> gRawModeResult(9999);
static std::atomic<int> gCalibrateResult(9999);
static std::atomic<int> gCalibrateResult2(9999);
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
static std::atomic<int> gCorrectedMin(0);
static std::atomic<int> gCorrectedMax(0);
static std::atomic<int> gCorrectedMean(0);
static std::atomic<int> gWindowLowInt(0);
static std::atomic<int> gWindowHighInt(0);
static std::atomic<long long> gFrameCounter(0);
static std::atomic<int> gDecoderMode(0); // 0 unknown, 1 YUV luma, 2 RAW16 LE
static std::atomic<int> gFpnState(0);    // 0 none, 1 shutter settling, 2 collecting, 3 ready, 4 failed
static std::atomic<int> gFpnFrames(0);
static std::atomic<int> gFpnMeanX100(0);
static std::atomic<int> gFpnStdX100(0);
static std::atomic<int> gSuppressFrames(0);

static bool isT2Pro() {
    return gVendorId == 0x3474 && gProductId == 0x6002;
}

static void resetFpn() {
    std::lock_guard<std::mutex> lock(gFpnMutex);
    gFpnReference.clear();
    gFpnAccum.clear();
    gFpnFrames.store(0);
    gFpnMeanX100.store(0);
    gFpnStdX100.store(0);
    gFpnState.store(0);
    gSuppressFrames.store(0);
}

static void resetDiagnostics() {
    gRawModeResult.store(9999);
    gCalibrateResult.store(9999);
    gCalibrateResult2.store(9999);
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
    gCorrectedMin.store(0);
    gCorrectedMax.store(0);
    gCorrectedMean.store(0);
    gWindowLowInt.store(0);
    gWindowHighInt.store(0);
    gFrameCounter.store(0);
    gDecoderMode.store(0);
    gWindowLow = -1.0f;
    gWindowHigh = -1.0f;
    gPreviousRaw.clear();
    resetFpn();
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

static bool collectFpnReference(const std::vector<uint16_t>& raw) {
    if (gFpnState.load() != 2) return false;
    std::lock_guard<std::mutex> lock(gFpnMutex);
    if (gFpnState.load() != 2) return false;

    if (gFpnAccum.size() != raw.size()) {
        gFpnAccum.assign(raw.size(), 0.0);
        gFpnFrames.store(0);
    }
    for (size_t i = 0; i < raw.size(); ++i) gFpnAccum[i] += raw[i];
    int frames = gFpnFrames.fetch_add(1) + 1;

    constexpr int targetFrames = 8;
    if (frames >= targetFrames) {
        gFpnReference.resize(raw.size());
        double mean = 0.0;
        for (size_t i = 0; i < raw.size(); ++i) {
            float v = static_cast<float>(gFpnAccum[i] / static_cast<double>(frames));
            gFpnReference[i] = v;
            mean += v;
        }
        mean /= std::max<size_t>(1, raw.size());
        double variance = 0.0;
        for (float v : gFpnReference) {
            double d = static_cast<double>(v) - mean;
            variance += d * d;
        }
        variance /= std::max<size_t>(1, gFpnReference.size());
        gFpnMeanX100.store(static_cast<int>(std::lround(mean * 100.0)));
        gFpnStdX100.store(static_cast<int>(std::lround(std::sqrt(variance) * 100.0)));
        gFpnAccum.clear();
        gFpnState.store(3);
        // 0x8000 keeps the shutter closed briefly. Suppress a few more frames so the user
        // never sees the closed-shutter reference image as an overlay.
        gSuppressFrames.store(16);
        gWindowLow = -1.0f;
        gWindowHigh = -1.0f;
        LOGI("FPN reference ready frames=%d mean=%.2f std=%.2f", frames, mean, std::sqrt(variance));
    }
    return true;
}

static bool decodeRaw16(const uint8_t* data, size_t step, int width, int height,
                        std::vector<jbyte>& out) {
    const int count = width * height;
    std::vector<uint16_t> raw(static_cast<size_t>(count));

    uint16_t rawMin = 0x3fff;
    uint16_t rawMax = 0;
    uint64_t rawSum = 0;
    uint64_t deltaSum = 0;
    const bool havePrevious = gPreviousRaw.size() == raw.size();

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * step;
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            // InfiRay radiometric samples are 14-bit values carried in a 16-bit LE word.
            uint16_t value = static_cast<uint16_t>(read16le(row + static_cast<size_t>(x) * 2) & 0x3fff);
            raw[i] = value;
            rawMin = std::min(rawMin, value);
            rawMax = std::max(rawMax, value);
            rawSum += value;
            if (havePrevious) {
                deltaSum += static_cast<uint64_t>(std::abs(static_cast<int>(value) - static_cast<int>(gPreviousRaw[i])));
            }
        }
    }

    gRawMin.store(rawMin);
    gRawMax.store(rawMax);
    gRawMean.store(count > 0 ? static_cast<int>(rawSum / static_cast<uint64_t>(count)) : 0);
    gRawDeltaX100.store(havePrevious && count > 0
                        ? static_cast<int>((deltaSum * 100ULL) / static_cast<uint64_t>(count))
                        : 0);
    gPreviousRaw = raw;

    // During shutter calibration the frame is intentionally uniform. Average several such
    // frames to learn the detector's fixed per-pixel offset pattern, then subtract that pattern
    // from every live frame. This is the step v0.3 lacked.
    if (gFpnState.load() == 1) return false;
    if (collectFpnReference(raw)) return false;

    int suppress = gSuppressFrames.load();
    if (suppress > 0) {
        gSuppressFrames.fetch_sub(1);
        return false;
    }

    std::vector<uint16_t> corrected(static_cast<size_t>(count));
    uint16_t corrMin = 0x3fff;
    uint16_t corrMax = 0;
    uint64_t corrSum = 0;
    bool haveRef = false;
    float refMean = gFpnMeanX100.load() / 100.0f;
    {
        std::lock_guard<std::mutex> lock(gFpnMutex);
        haveRef = gFpnState.load() == 3 && gFpnReference.size() == raw.size();
        for (int i = 0; i < count; ++i) {
            float value = static_cast<float>(raw[static_cast<size_t>(i)]);
            if (haveRef) value = value - gFpnReference[static_cast<size_t>(i)] + refMean;
            int v = static_cast<int>(std::lround(value));
            v = std::max(0, std::min(16383, v));
            corrected[static_cast<size_t>(i)] = static_cast<uint16_t>(v);
            corrMin = std::min(corrMin, static_cast<uint16_t>(v));
            corrMax = std::max(corrMax, static_cast<uint16_t>(v));
            corrSum += static_cast<uint16_t>(v);
        }
    }
    gCorrectedMin.store(corrMin);
    gCorrectedMax.store(corrMax);
    gCorrectedMean.store(count > 0 ? static_cast<int>(corrSum / static_cast<uint64_t>(count)) : 0);

    // 14-bit histogram gives much finer AGC than the old 16-value bins, which matters after
    // fixed-pattern subtraction because useful scene contrast can be only a few hundred counts.
    thread_local std::array<int, 16384> hist;
    hist.fill(0);
    for (uint16_t v : corrected) hist[v]++;

    const int lowTarget = std::max(1, count / 100);
    const int highTarget = std::max(lowTarget + 1, count - count / 100);
    int cumulative = 0;
    int low = 0;
    int high = 16383;
    for (int i = 0; i < 16384; ++i) {
        cumulative += hist[static_cast<size_t>(i)];
        if (cumulative >= lowTarget) { low = i; break; }
    }
    cumulative = 0;
    for (int i = 0; i < 16384; ++i) {
        cumulative += hist[static_cast<size_t>(i)];
        if (cumulative >= highTarget) { high = i; break; }
    }

    float lowF = static_cast<float>(low);
    float highF = static_cast<float>(high);
    if (highF - lowF < 48.0f) {
        float mid = (highF + lowF) * 0.5f;
        lowF = mid - 24.0f;
        highF = mid + 24.0f;
    }

    if (gWindowLow < 0.0f || gWindowHigh < 0.0f) {
        gWindowLow = lowF;
        gWindowHigh = highF;
    } else {
        constexpr float alpha = 0.20f;
        gWindowLow += (lowF - gWindowLow) * alpha;
        gWindowHigh += (highF - gWindowHigh) * alpha;
    }
    gWindowLowInt.store(static_cast<int>(gWindowLow));
    gWindowHighInt.store(static_cast<int>(gWindowHigh));

    const float denom = std::max(20.0f, gWindowHigh - gWindowLow);
    for (int i = 0; i < count; ++i) {
        float scaled = (static_cast<float>(corrected[static_cast<size_t>(i)]) - gWindowLow) * 255.0f / denom;
        int value = static_cast<int>(scaled + 0.5f);
        value = std::max(0, std::min(255, value));
        out[static_cast<size_t>(i)] = static_cast<jbyte>(static_cast<uint8_t>(value));
    }
    return true;
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

    bool deliver = true;
    const bool raw16 = (width == 256 && physicalHeight >= 196) && isT2Pro();
    if (raw16) {
        gDecoderMode.store(2);
        deliver = decodeRaw16(data, step, width, imageHeight, luma);
    } else {
        gDecoderMode.store(1);
        decodeYuvLuma(data, step, width, imageHeight, frame->frame_format, luma);
    }
    if (!deliver) return;

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

static bool performFpnCalibration() {
    if (!gDevh || !gStreaming.load() || !isT2Pro()) return false;
    resetFpn();
    gFpnState.store(1);

    // Match the known-working IR-Py-Thermal raw calibration sequence: wait for the stream,
    // close the shutter with 0x8000, wait ~300 ms, issue 0x8000 again to keep it closed,
    // then capture a shutter reference frame. We average 8 frames for lower noise.
    usleep(500000);
    uvc_error_t r1 = uvc_set_zoom_abs(gDevh, 0x8000);
    gCalibrateResult.store(static_cast<int>(r1));
    usleep(300000);
    gFpnState.store(2);
    uvc_error_t r2 = uvc_set_zoom_abs(gDevh, 0x8000);
    gCalibrateResult2.store(static_cast<int>(r2));
    LOGI("FPN shutter sequence r1=%d r2=%d", r1, r2);

    for (int i = 0; i < 60; ++i) {
        int state = gFpnState.load();
        if (state == 3) return true;
        usleep(20000);
    }
    gFpnState.store(4);
    LOGE("FPN reference capture timed out frames=%d", gFpnFrames.load());
    return false;
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
        uvc_error_t rawResult = uvc_set_zoom_abs(gDevh, 0x8004);
        gRawModeResult.store(static_cast<int>(rawResult));
        uint16_t zoom = 0;
        uvc_error_t readResult = uvc_get_zoom_abs(gDevh, &zoom, UVC_GET_CUR);
        gZoomReadResult.store(static_cast<int>(readResult));
        gZoomReadback.store(static_cast<int>(zoom));
        LOGI("T2 Pro raw command 0x8004 result=%d read=%d current=0x%04x", rawResult, readResult, zoom);
        usleep(120000);
    }
    LOGI("UVC camera opened VID=%04x PID=%04x", gVendorId, gProductId);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ikegami99_thermaledge_ThermalCamera_nativeStartStream(JNIEnv* env, jobject, jobject callback) {
    std::unique_lock<std::mutex> guard(gMutex);
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
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
        if (result != UVC_SUCCESS) result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
    } else {
        result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 192, 25);
        if (result != UVC_SUCCESS) result = uvc_get_stream_ctrl_format_size(gDevh, &ctrl, UVC_FRAME_FORMAT_YUYV, 256, 196, 25);
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
    guard.unlock();

    if (isT2Pro()) {
        bool ok = performFpnCalibration();
        LOGI("Automatic FPN calibration %s", ok ? "READY" : "FAILED");
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
    if (gDevh) { uvc_close(gDevh); gDevh = nullptr; }
    if (gWrappedFd >= 0) { close(gWrappedFd); gWrappedFd = -1; }
    if (gCtx) { uvc_exit(gCtx); gCtx = nullptr; }
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
    char buffer[1024];
    const int mode = gDecoderMode.load();
    const char* modeName = mode == 2 ? "RAW16_LE_FPN" : (mode == 1 ? "YUV_LUMA" : "UNKNOWN");
    const int fpn = gFpnState.load();
    const char* fpnName = fpn == 3 ? "READY" : (fpn == 2 ? "COLLECTING" : (fpn == 1 ? "SHUTTER" : (fpn == 4 ? "FAILED" : "NONE")));
    std::snprintf(buffer, sizeof(buffer),
                  "VID=%04X PID=%04X rawCmd=%d nuc1=%d nuc2=%d zoomRead=%d zoom=0x%04X "
                  "frame=%dx%d fmt=%d step=%d decoder=%s count=%lld "
                  "raw[min=%d max=%d mean=%d delta=%.2f] "
                  "fpn=%s frames=%d refMean=%.2f refStd=%.2f corr[min=%d max=%d mean=%d] window=%d..%d",
                  gVendorId, gProductId,
                  gRawModeResult.load(), gCalibrateResult.load(), gCalibrateResult2.load(),
                  gZoomReadResult.load(), gZoomReadback.load() & 0xffff,
                  gFrameWidth.load(), gFrameHeight.load(), gFrameFormat.load(), gFrameStep.load(),
                  modeName, static_cast<long long>(gFrameCounter.load()),
                  gRawMin.load(), gRawMax.load(), gRawMean.load(), gRawDeltaX100.load() / 100.0,
                  fpnName, gFpnFrames.load(), gFpnMeanX100.load() / 100.0, gFpnStdX100.load() / 100.0,
                  gCorrectedMin.load(), gCorrectedMax.load(), gCorrectedMean.load(),
                  gWindowLowInt.load(), gWindowHighInt.load());
    return env->NewStringUTF(buffer);
}
