package com.ikegami99.thermaledge;

import android.Manifest;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.camera.core.Camera;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.Preview;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.camera.view.PreviewView;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.common.util.concurrent.ListenableFuture;

import java.util.Locale;

public final class MainActivity extends AppCompatActivity {
    private static final String ACTION_USB_PERMISSION = "com.ikegami99.thermaledge.USB_PERMISSION";
    private static final int GREEN = Color.rgb(101, 255, 138);
    private static final int DIM_GREEN = Color.rgb(93, 155, 108);

    private static final int DEFAULT_THRESHOLD = 55;
    private static final int DEFAULT_OPACITY = 80;
    private static final int DEFAULT_SCALE = 100;
    private static final int DEFAULT_OFFSET_X = 0;
    private static final int DEFAULT_OFFSET_Y = 0;
    private static final int DEFAULT_ROTATION = 0;
    private static final int DEFAULT_RGB_ZOOM = 55;

    private PreviewView previewView;
    private ThermalEdgeView edgeView;
    private TextView usbStatus;
    private TextView fpsStatus;
    private TextView alignStatus;
    private Button connectButton;
    private Button updateButton;

    private SeekBar thresholdBar;
    private SeekBar opacityBar;
    private SeekBar scaleBar;
    private SeekBar offsetXBar;
    private SeekBar offsetYBar;
    private SeekBar rotationBar;
    private SeekBar zoomBar;

    private UsbManager usbManager;
    private final ThermalCamera thermalCamera = new ThermalCamera();
    private Camera rgbCamera;
    private UpdateManager updateManager;

    private long fpsEpoch;
    private int fpsFrames;
    private float offsetX;
    private float offsetY;

    private final ActivityResultLauncher<String> cameraPermission = registerForActivityResult(
            new ActivityResultContracts.RequestPermission(), granted -> {
                if (granted) startRgbCamera();
                else usbStatus.setText("RGB CAMERA : PERMISSION DENIED");
            });

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (!ACTION_USB_PERMISSION.equals(intent.getAction())) return;
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
            if (granted && device != null) openThermal(device);
            else setUsbState("THERMAL : USB PERMISSION DENIED", false);
        }
    };

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        buildUi();
        updateManager = new UpdateManager(this, this::onUpdateStatus);
        ContextCompat.registerReceiver(this, usbReceiver, new IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED);
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startRgbCamera();
        } else {
            cameraPermission.launch(Manifest.permission.CAMERA);
        }
    }

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(5, 8, 6));

        previewView = new PreviewView(this);
        previewView.setScaleType(PreviewView.ScaleType.FILL_CENTER);
        root.addView(previewView, new FrameLayout.LayoutParams(-1, -1));

        edgeView = new ThermalEdgeView(this);
        root.addView(edgeView, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.VERTICAL);
        top.setPadding(dp(16), dp(10), dp(16), dp(10));
        top.setBackgroundColor(0xC90A100C);
        TextView title = text("THERMAL EDGE", 22, GREEN);
        title.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        top.addView(title);
        usbStatus = text("THERMAL : SEARCHING", 12, GREEN);
        fpsStatus = text("IR FPS  : --", 12, DIM_GREEN);
        alignStatus = text("ALIGN   : X +0.00  Y +0.00  S 1.00", 12, DIM_GREEN);
        top.addView(usbStatus);
        top.addView(fpsStatus);
        top.addView(alignStatus);
        FrameLayout.LayoutParams topParams = new FrameLayout.LayoutParams(-1, -2, Gravity.TOP);
        root.addView(top, topParams);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(12));
        panel.setBackgroundColor(0xDD07100A);

        connectButton = new Button(this);
        connectButton.setText("CONNECT T2 PRO / UVC");
        connectButton.setTextColor(Color.BLACK);
        connectButton.setBackgroundColor(GREEN);
        connectButton.setOnClickListener(v -> findThermalCamera());
        panel.addView(connectButton, new LinearLayout.LayoutParams(-1, dp(44)));

        LinearLayout actionRow = new LinearLayout(this);
        actionRow.setOrientation(LinearLayout.HORIZONTAL);
        actionRow.setPadding(0, dp(6), 0, 0);

        Button resetButton = new Button(this);
        resetButton.setText("RESET VALUES");
        resetButton.setTextColor(Color.BLACK);
        resetButton.setBackgroundColor(DIM_GREEN);
        resetButton.setOnClickListener(v -> resetControls());
        LinearLayout.LayoutParams actionParams = new LinearLayout.LayoutParams(0, dp(42), 1f);
        actionParams.setMarginEnd(dp(4));
        actionRow.addView(resetButton, actionParams);

        updateButton = new Button(this);
        updateButton.setText("CHECK UPDATE");
        updateButton.setTextColor(Color.BLACK);
        updateButton.setBackgroundColor(GREEN);
        updateButton.setOnClickListener(v -> updateManager.checkForUpdate());
        LinearLayout.LayoutParams updateParams = new LinearLayout.LayoutParams(0, dp(42), 1f);
        updateParams.setMarginStart(dp(4));
        actionRow.addView(updateButton, updateParams);
        panel.addView(actionRow);

        thresholdBar = addSlider(panel, "EDGE THRESHOLD", 10, 180, DEFAULT_THRESHOLD,
                value -> edgeView.setThreshold(value));
        opacityBar = addSlider(panel, "EDGE OPACITY", 0, 100, DEFAULT_OPACITY,
                value -> edgeView.setOpacity(value / 100f));
        scaleBar = addSlider(panel, "THERMAL SCALE", 20, 240, DEFAULT_SCALE, value -> {
            edgeView.setScaleFactor(value / 100f);
            updateAlign(value / 100f);
        });
        offsetXBar = addSlider(panel, "OFFSET X", -100, 100, DEFAULT_OFFSET_X, value -> {
            offsetX = value / 200f;
            edgeView.setOffset(offsetX, offsetY);
            updateAlign(null);
        });
        offsetYBar = addSlider(panel, "OFFSET Y", -100, 100, DEFAULT_OFFSET_Y, value -> {
            offsetY = value / 200f;
            edgeView.setOffset(offsetX, offsetY);
            updateAlign(null);
        });
        rotationBar = addSlider(panel, "ROTATION", -30, 30, DEFAULT_ROTATION,
                value -> edgeView.setRotationDegrees(value));
        zoomBar = addSlider(panel, "RGB ZOOM", 0, 100, DEFAULT_RGB_ZOOM, value -> {
            if (rgbCamera != null) rgbCamera.getCameraControl().setLinearZoom(value / 100f);
        });

        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        root.addView(panel, panelParams);

        ViewCompat.setOnApplyWindowInsetsListener(root, (view, insets) -> {
            Insets bars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            topParams.topMargin = bars.top;
            panelParams.bottomMargin = bars.bottom;
            top.setLayoutParams(topParams);
            panel.setLayoutParams(panelParams);
            return insets;
        });

        setContentView(root);
        ViewCompat.requestApplyInsets(root);
    }

    private interface ValueListener { void onValue(int value); }

    private SeekBar addSlider(LinearLayout parent, String name, int min, int max, int initial, ValueListener listener) {
        TextView label = text(name + "  " + initial, 11, GREEN);
        label.setPadding(0, dp(5), 0, 0);
        parent.addView(label);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(initial - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int value = progress + min;
                label.setText(name + "  " + value);
                listener.onValue(value);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        parent.addView(bar, new LinearLayout.LayoutParams(-1, dp(28)));
        return bar;
    }

    private void resetControls() {
        setSliderValue(thresholdBar, 10, DEFAULT_THRESHOLD);
        setSliderValue(opacityBar, 0, DEFAULT_OPACITY);
        setSliderValue(scaleBar, 20, DEFAULT_SCALE);
        setSliderValue(offsetXBar, -100, DEFAULT_OFFSET_X);
        setSliderValue(offsetYBar, -100, DEFAULT_OFFSET_Y);
        setSliderValue(rotationBar, -30, DEFAULT_ROTATION);
        setSliderValue(zoomBar, 0, DEFAULT_RGB_ZOOM);
        Toast.makeText(this, "表示パラメータを初期値に戻しました", Toast.LENGTH_SHORT).show();
    }

    private void setSliderValue(SeekBar bar, int min, int value) {
        if (bar != null) bar.setProgress(value - min);
    }

    private void onUpdateStatus(String status) {
        runOnUiThread(() -> {
            if (updateButton != null) updateButton.setText(status);
            if (status.startsWith("UP TO DATE") || status.startsWith("UPDATE ERROR")) {
                Toast.makeText(this, status, Toast.LENGTH_LONG).show();
                updateButton.postDelayed(() -> updateButton.setText("CHECK UPDATE"), 2500);
            }
        });
    }

    private TextView text(String value, int sp, int color) {
        TextView tv = new TextView(this);
        tv.setText(value);
        tv.setTextSize(sp);
        tv.setTextColor(color);
        tv.setFontFeatureSettings("tnum");
        return tv;
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private void startRgbCamera() {
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(this);
        future.addListener(() -> {
            try {
                ProcessCameraProvider provider = future.get();
                Preview preview = new Preview.Builder().build();
                preview.setSurfaceProvider(previewView.getSurfaceProvider());
                provider.unbindAll();
                rgbCamera = provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, preview);
                rgbCamera.getCameraControl().setLinearZoom(DEFAULT_RGB_ZOOM / 100f);
            } catch (Exception e) {
                usbStatus.setText("RGB CAMERA : ERROR " + e.getClass().getSimpleName());
            }
        }, ContextCompat.getMainExecutor(this));
    }

    private void findThermalCamera() {
        setUsbState("THERMAL : SCANNING USB", false);
        UsbDevice chosen = null;
        for (UsbDevice device : usbManager.getDeviceList().values()) {
            if (isUvc(device)) { chosen = device; break; }
        }
        if (chosen == null) {
            setUsbState("THERMAL : NO UVC DEVICE", false);
            return;
        }
        if (usbManager.hasPermission(chosen)) {
            openThermal(chosen);
        } else {
            PendingIntent permission = PendingIntent.getBroadcast(
                    this, 0,
                    new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName()),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            usbManager.requestPermission(chosen, permission);
            setUsbState("THERMAL : WAITING USB PERMISSION", false);
        }
    }

    private boolean isUvc(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface iface = device.getInterface(i);
            if (iface.getInterfaceClass() == UsbConstants.USB_CLASS_VIDEO) return true;
        }
        String product = device.getProductName();
        return product != null && (product.toLowerCase(Locale.US).contains("t2pro")
                || product.toLowerCase(Locale.US).contains("thermal")
                || product.toLowerCase(Locale.US).contains("infiray"));
    }

    private void openThermal(@NonNull UsbDevice device) {
        setUsbState("THERMAL : OPENING " + safeName(device), false);
        UsbDeviceConnection connection = usbManager.openDevice(device);
        if (connection == null || !thermalCamera.open(device, connection)) {
            setUsbState("THERMAL : OPEN FAILED", false);
            return;
        }
        fpsEpoch = System.nanoTime();
        fpsFrames = 0;
        boolean started = thermalCamera.start((luma, width, height) -> {
            edgeView.submitLuma(luma, width, height);
            fpsFrames++;
            long now = System.nanoTime();
            if (now - fpsEpoch >= 1_000_000_000L) {
                final int fps = fpsFrames;
                fpsFrames = 0;
                fpsEpoch = now;
                runOnUiThread(() -> fpsStatus.setText("IR FPS  : " + fps + "   " + width + "x" + height + "  RAW16"));
            }
        });
        if (started) setUsbState("THERMAL : CONNECTED  " + safeName(device), true);
        else setUsbState("THERMAL : STREAM FAILED", false);
    }

    private String safeName(UsbDevice device) {
        String name = device.getProductName();
        return name == null ? String.format(Locale.US, "%04X:%04X", device.getVendorId(), device.getProductId()) : name;
    }

    private void setUsbState(String state, boolean connected) {
        runOnUiThread(() -> {
            usbStatus.setText(state);
            usbStatus.setTextColor(connected ? GREEN : Color.rgb(255, 184, 92));
            connectButton.setText(connected ? "T2 PRO CONNECTED" : "CONNECT T2 PRO / UVC");
        });
    }

    private float lastScale = 1f;
    private void updateAlign(Float newScale) {
        if (newScale != null) lastScale = newScale;
        alignStatus.setText(String.format(Locale.US, "ALIGN   : X %+1.2f  Y %+1.2f  S %.2f", offsetX, offsetY, lastScale));
    }

    @Override protected void onResume() {
        super.onResume();
        if (updateManager != null) updateManager.resumePendingInstall();
    }

    @Override protected void onDestroy() {
        thermalCamera.close();
        if (updateManager != null) updateManager.close();
        unregisterReceiver(usbReceiver);
        super.onDestroy();
    }
}
