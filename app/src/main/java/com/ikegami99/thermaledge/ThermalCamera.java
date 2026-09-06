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
        String opening = "Opening UVC VID=" + String.format("%04X", device.getVendorId())
                + " PID=" + String.format("%04X", device.getProductId())
                + " product=" + device.getProductName() + " fd=" + fd;
        Log.i(TAG, opening);
        AppLog.i("USB", opening);

        open = nativeOpen(fd, device.getVendorId(), device.getProductId());
        AppLog.i("NATIVE", "open=" + open + " | " + diagnostics());
        if (!open) {
            connection.close();
            this.connection = null;
        }
        return open;
    }

    boolean start(FrameCallback callback) {
        if (!open) return false;
        this.callback = callback;
        boolean started = nativeStartStream(new NativeCallback());
        AppLog.i("NATIVE", "stream start=" + started + " | " + diagnostics());
        return started;
    }

    boolean isStreaming() {
        return nativeIsStreaming();
    }

    String diagnostics() {
        try {
            String value = nativeGetDiagnostics();
            return value == null ? "native diagnostics unavailable" : value;
        } catch (Throwable t) {
            return "native diagnostics error=" + t.getClass().getSimpleName() + ":" + t.getMessage();
        }
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

    private native boolean nativeOpen(int fd, int vendorId, int productId);
    private native boolean nativeStartStream(Object callback);
    private native void nativeStopStream();
    private native void nativeClose();
    private native boolean nativeIsStreaming();
    private native String nativeGetDiagnostics();
}
