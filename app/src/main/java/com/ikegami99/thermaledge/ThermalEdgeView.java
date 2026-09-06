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
    private int threshold = 70;
    private float opacity = 0.9f;
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

    public void setThreshold(int value) { threshold = Math.max(5, Math.min(255, value)); }
    public void setOpacity(float value) { opacity = Math.max(0f, Math.min(1f, value)); invalidate(); }
    public void setScaleFactor(float value) { scale = Math.max(0.15f, Math.min(3f, value)); invalidate(); }
    public void setRotationDegrees(float value) { rotation = value; invalidate(); }
    public void setOffset(float xNormalized, float yNormalized) { offsetX = xNormalized; offsetY = yNormalized; invalidate(); }

    public void submitLuma(byte[] luma, int width, int height) {
        if (luma == null || width < 3 || height < 3 || luma.length < width * height) return;
        synchronized (frameLock) {
            if (edgeBitmap == null || edgeBitmap.getWidth() != width || edgeBitmap.getHeight() != height) {
                edgeBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                pixels = new int[width * height];
            }
            java.util.Arrays.fill(pixels, 0);
            int t = threshold;
            for (int y = 1; y < height - 1; y++) {
                int row = y * width;
                for (int x = 1; x < width - 1; x++) {
                    int i = row + x;
                    int tl = luma[i - width - 1] & 0xff;
                    int tc = luma[i - width] & 0xff;
                    int tr = luma[i - width + 1] & 0xff;
                    int ml = luma[i - 1] & 0xff;
                    int mr = luma[i + 1] & 0xff;
                    int bl = luma[i + width - 1] & 0xff;
                    int bc = luma[i + width] & 0xff;
                    int br = luma[i + width + 1] & 0xff;
                    int gx = -tl - (ml << 1) - bl + tr + (mr << 1) + br;
                    int gy = -tl - (tc << 1) - tr + bl + (bc << 1) + br;
                    int mag = (Math.abs(gx) + Math.abs(gy)) >> 2;
                    if (mag >= t) {
                        int a = Math.min(255, 100 + mag);
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
