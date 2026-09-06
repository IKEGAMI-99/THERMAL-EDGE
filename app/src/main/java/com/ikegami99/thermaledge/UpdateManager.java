package com.ikegami99.thermaledge;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

import androidx.core.content.FileProvider;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

final class UpdateManager {
    interface Listener {
        void onStatus(String status);
    }

    private static final String RELEASE_API =
            "https://api.github.com/repos/IKEGAMI-99/THERMAL-EDGE/releases/latest";

    private final Activity activity;
    private final Listener listener;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean(false);
    private volatile File pendingApk;

    UpdateManager(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
    }

    void checkForUpdate() {
        if (!busy.compareAndSet(false, true)) return;
        status("CHECKING...");
        executor.execute(() -> {
            try {
                JSONObject release = readJson(RELEASE_API);
                String tag = release.optString("tag_name", "");
                String remoteVersion = normalizeVersion(tag);
                String localVersion = getLocalVersion();

                if (remoteVersion.isEmpty()) {
                    throw new IllegalStateException("release has no version tag");
                }
                if (compareVersions(remoteVersion, localVersion) <= 0) {
                    status("UP TO DATE v" + localVersion);
                    return;
                }

                String apkUrl = findApkUrl(release.optJSONArray("assets"));
                if (apkUrl == null) {
                    throw new IllegalStateException("release has no APK asset");
                }

                status("DOWNLOADING...");
                File apk = downloadApk(apkUrl, remoteVersion);
                pendingApk = apk;
                status("INSTALL v" + remoteVersion);
                activity.runOnUiThread(() -> requestInstall(apk));
            } catch (Exception e) {
                status("UPDATE ERROR");
            } finally {
                busy.set(false);
            }
        });
    }

    void resumePendingInstall() {
        File apk = pendingApk;
        if (apk == null || !apk.exists()) return;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O || activity.getPackageManager().canRequestPackageInstalls()) {
            pendingApk = null;
            install(apk);
        }
    }

    void close() {
        executor.shutdownNow();
    }

    private JSONObject readJson(String address) throws Exception {
        HttpURLConnection connection = open(address);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "THERMAL-EDGE-Android");
        try (InputStream input = new BufferedInputStream(connection.getInputStream())) {
            byte[] data = readAll(input);
            return new JSONObject(new String(data, java.nio.charset.StandardCharsets.UTF_8));
        } finally {
            connection.disconnect();
        }
    }

    private File downloadApk(String address, String version) throws Exception {
        File dir = activity.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
        if (dir == null) dir = activity.getFilesDir();
        if (!dir.exists() && !dir.mkdirs()) throw new IllegalStateException("cannot create update directory");
        File target = new File(dir, "thermal-edge-v" + version + ".apk");

        HttpURLConnection connection = open(address);
        connection.setRequestProperty("User-Agent", "THERMAL-EDGE-Android");
        long total = connection.getContentLengthLong();
        try (InputStream input = new BufferedInputStream(connection.getInputStream());
             FileOutputStream output = new FileOutputStream(target, false)) {
            byte[] buffer = new byte[64 * 1024];
            long done = 0;
            int lastPercent = -10;
            int n;
            while ((n = input.read(buffer)) >= 0) {
                if (n == 0) continue;
                output.write(buffer, 0, n);
                done += n;
                if (total > 0) {
                    int percent = (int) (done * 100L / total);
                    if (percent >= lastPercent + 10) {
                        lastPercent = percent;
                        status(String.format(Locale.US, "UPDATE %d%%", Math.min(100, percent)));
                    }
                }
            }
            output.flush();
        } finally {
            connection.disconnect();
        }
        return target;
    }

    private void requestInstall(File apk) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !activity.getPackageManager().canRequestPackageInstalls()) {
            pendingApk = apk;
            status("ALLOW INSTALL");
            Intent settings = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + activity.getPackageName()));
            activity.startActivity(settings);
            return;
        }
        pendingApk = null;
        install(apk);
    }

    private void install(File apk) {
        Uri uri = FileProvider.getUriForFile(
                activity,
                activity.getPackageName() + ".provider",
                apk);
        Intent install = new Intent(Intent.ACTION_VIEW);
        install.setDataAndType(uri, "application/vnd.android.package-archive");
        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        activity.startActivity(install);
    }

    private String getLocalVersion() throws Exception {
        PackageInfo info = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0);
        return info.versionName == null ? "0.0.0" : info.versionName;
    }

    private String findApkUrl(JSONArray assets) {
        if (assets == null) return null;
        String fallback = null;
        for (int i = 0; i < assets.length(); i++) {
            JSONObject asset = assets.optJSONObject(i);
            if (asset == null) continue;
            String name = asset.optString("name", "");
            String url = asset.optString("browser_download_url", "");
            if (!name.toLowerCase(Locale.US).endsWith(".apk") || url.isEmpty()) continue;
            if (name.equalsIgnoreCase("thermal-edge.apk")) return url;
            if (fallback == null) fallback = url;
        }
        return fallback;
    }

    private HttpURLConnection open(String address) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(12000);
        connection.setReadTimeout(30000);
        connection.setInstanceFollowRedirects(true);
        connection.setUseCaches(false);
        return connection;
    }

    private byte[] readAll(InputStream input) throws Exception {
        java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int n;
        while ((n = input.read(buffer)) >= 0) {
            if (n > 0) output.write(buffer, 0, n);
        }
        return output.toByteArray();
    }

    private String normalizeVersion(String value) {
        if (value == null) return "";
        String v = value.trim();
        if (v.startsWith("v") || v.startsWith("V")) v = v.substring(1);
        int dash = v.indexOf('-');
        if (dash >= 0) v = v.substring(0, dash);
        return v;
    }

    private int compareVersions(String a, String b) {
        String[] aa = a.split("\\.");
        String[] bb = b.split("\\.");
        int length = Math.max(aa.length, bb.length);
        for (int i = 0; i < length; i++) {
            int av = i < aa.length ? leadingNumber(aa[i]) : 0;
            int bv = i < bb.length ? leadingNumber(bb[i]) : 0;
            if (av != bv) return Integer.compare(av, bv);
        }
        return 0;
    }

    private int leadingNumber(String value) {
        int result = 0;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c < '0' || c > '9') break;
            result = result * 10 + (c - '0');
        }
        return result;
    }

    private void status(String value) {
        activity.runOnUiThread(() -> listener.onStatus(value));
    }
}
