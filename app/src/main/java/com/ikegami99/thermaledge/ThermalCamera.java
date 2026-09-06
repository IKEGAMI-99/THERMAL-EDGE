package com.ikegami99.thermaledge;

import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.util.Log;

final class ThermalCamera {
    private static final String TAG = "ThermalCamera";

    static {
        System.loadLibrary("thermaledge");
    }

    interface FrameCallback {
        void onFrame(byte[] luma, int width, int height);
    }

    private UsbDeviceConnection connection;
    private boolean open;
    private FrameCallback callback;

    boolean open(UsbDevice device, UsbDeviceConnection connection) {
        close();
        this.connection = connection;
        int fd = connection.getFileDescriptor();
        Log.i(TAG, "Opening UVC thermal camera VID=" + Integer.toHexString(device.getVendorId())
                + " PID=" + Integer.toHexString(device.getProductId()) + " fd=" + fd);
        open = nativeOpen(fd);
        if (!open) {
            connection.close();
            this.connection = null;
        }
        return open;
    }

    boolean start(FrameCallback callback) {
        if (!open) return false;
        this.callback = callback;
        return nativeStartStream(new NativeCallback());
    }

    boolean isStreaming() {
        return nativeIsStreaming();
    }

    void close() {
        callback = null;
        nativeStopStream();
        nativeClose();
        open = false;
        if (connection != null) {
            try { connection.close(); } catch (Exception ignored) {}
            connection = null;
        }
    }

    private final class NativeCallback {
        @SuppressWarnings("unused")
        public void onFrame(byte[] luma, int width, int height) {
            FrameCallback cb = callback;
            if (cb != null) cb.onFrame(luma, width, height);
        }
    }

    private native boolean nativeOpen(int fd);
    private native boolean nativeStartStream(Object callback);
    private native void nativeStopStream();
    private native void nativeClose();
    private native boolean nativeIsStreaming();
}
