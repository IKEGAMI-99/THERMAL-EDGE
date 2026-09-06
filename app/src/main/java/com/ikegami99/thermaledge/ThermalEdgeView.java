package com.ikegami99.thermaledge;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

public final class ThermalEdgeView extends View {
    private final Object frameLock = new Object();
    private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint hudPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private Bitmap edgeBitmap;
    private int[] pixels;
    private int[] blurred;
    private int threshold = 55;
    private float opacity = 0.8f;
    private float scale = 1.0f;
    private float rotation = 0f;
    private float offsetX = 0f;
    private float offsetY = 0f;

    public ThermalEdgeView(Context context) { super(context); init(); }
    public ThermalEdgeView(Context context, AttributeSet attrs) { super(context, attrs); init(); }

    private void init() {
        setBackgroundColor(0x00000000);
        hudPaint.setStyle(Paint.Style.STROKE);
        hudPaint.setStrokeWidth(2f);
        hudPaint.setColor(0x9965FF8A);
    }

    public void setThreshold(int value) { threshold = Math.max(10, Math.min(255, value)); }
    public void setOpacity(float value) { opacity = Math.max(0f, Math.min(1f, value)); invalidate(); }
    public void setScaleFactor(float value) { scale = Math.max(0.15f, Math.min(3f, value)); invalidate(); }
    public void setRotationDegrees(float value) { rotation = value; invalidate(); }
    public void setOffset(float xNormalized, float yNormalized) { offsetX = xNormalized; offsetY = yNormalized; invalidate(); }

    public void submitLuma(byte[] luma, int width, int height) {
        if (luma == null || width < 5 || height < 5 || luma.length < width * height) return;
        synchronized (frameLock) {
            int size = width * height;
            if (edgeBitmap == null || edgeBitmap.getWidth() != width || edgeBitmap.getHeight() != height) {
                edgeBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                pixels = new int[size];
                blurred = new int[size];
            }

            java.util.Arrays.fill(pixels, 0);
            java.util.Arrays.fill(blurred, 0);

            // Small Gaussian-like blur first. Raw thermal sensors contain fixed-pattern and temporal
            // noise; running Sobel directly on that noise turns the entire frame into a green block.
            for (int y = 1; y < height - 1; y++) {
                int row = y * width;
                for (int x = 1; x < width - 1; x++) {
                    int i = row + x;
                    int c = luma[i] & 0xff;
                    int n = luma[i - width] & 0xff;
                    int s = luma[i + width] & 0xff;
                    int w = luma[i - 1] & 0xff;
                    int e = luma[i + 1] & 0xff;
                    int nw = luma[i - width - 1] & 0xff;
                    int ne = luma[i - width + 1] & 0xff;
                    int sw = luma[i + width - 1] & 0xff;
                    int se = luma[i + width + 1] & 0xff;
                    blurred[i] = (c * 4 + (n + s + w + e) * 2 + nw + ne + sw + se) >> 4;
                }
            }

            int t = threshold;
            for (int y = 2; y < height - 2; y++) {
                int row = y * width;
                for (int x = 2; x < width - 2; x++) {
                    int i = row + x;
                    int tl = blurred[i - width - 1];
                    int tc = blurred[i - width];
                    int tr = blurred[i - width + 1];
                    int ml = blurred[i - 1];
                    int mr = blurred[i + 1];
                    int bl = blurred[i + width - 1];
                    int bc = blurred[i + width];
                    int br = blurred[i + width + 1];
                    int gx = -tl - (ml << 1) - bl + tr + (mr << 1) + br;
                    int gy = -tl - (tc << 1) - tr + bl + (bc << 1) + br;
                    int mag = (Math.abs(gx) + Math.abs(gy)) >> 2;
                    if (mag >= t) {
                        int a = Math.min(220, 42 + (mag - t) * 4);
                        pixels[i] = (a << 24) | 0x0065FF8A;
                    }
                }
            }
            edgeBitmap.setPixels(pixels, 0, width, 0, 0, width, height);
        }
        postInvalidateOnAnimation();
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        Bitmap bitmap;
        synchronized (frameLock) { bitmap = edgeBitmap; }
        if (bitmap == null) {
            drawReticle(canvas);
            return;
        }

        float base = Math.min(getWidth() / (float) bitmap.getWidth(), getHeight() / (float) bitmap.getHeight());
        float s = base * scale;
        bitmapPaint.setAlpha((int) (opacity * 255));

        canvas.save();
        canvas.translate(getWidth() * (0.5f + offsetX), getHeight() * (0.5f + offsetY));
        canvas.rotate(rotation);
        canvas.scale(s, s);
        canvas.drawBitmap(bitmap, -bitmap.getWidth() / 2f, -bitmap.getHeight() / 2f, bitmapPaint);
        canvas.restore();
        drawReticle(canvas);
    }

    private void drawReticle(Canvas canvas) {
        float cx = getWidth() / 2f;
        float cy = getHeight() / 2f;
        canvas.drawLine(cx - 18, cy, cx - 5, cy, hudPaint);
        canvas.drawLine(cx + 5, cy, cx + 18, cy, hudPaint);
        canvas.drawLine(cx, cy - 18, cx, cy - 5, hudPaint);
        canvas.drawLine(cx, cy + 5, cx, cy + 18, hudPaint);
    }
}
