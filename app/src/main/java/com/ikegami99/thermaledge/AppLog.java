package com.ikegami99.thermaledge;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class AppLog {
    private static final int MAX_CHARS = 512_000;
    private static final StringBuilder BUFFER = new StringBuilder();
    private static final SimpleDateFormat FORMAT = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US);

    private AppLog() {}

    static synchronized void clear() {
        BUFFER.setLength(0);
    }

    static synchronized void i(String tag, String message) {
        append("I", tag, message);
    }

    static synchronized void w(String tag, String message) {
        append("W", tag, message);
    }

    static synchronized void e(String tag, String message) {
        append("E", tag, message);
    }

    private static void append(String level, String tag, String message) {
        String safe = message == null ? "null" : message.replace('\n', ' ');
        BUFFER.append(FORMAT.format(new Date()))
                .append(' ')
                .append(level)
                .append('/')
                .append(tag)
                .append("  ")
                .append(safe)
                .append('\n');

        if (BUFFER.length() > MAX_CHARS) {
            int trim = BUFFER.length() - MAX_CHARS;
            int newline = BUFFER.indexOf("\n", trim);
            BUFFER.delete(0, newline >= 0 ? newline + 1 : trim);
        }
    }

    static synchronized String snapshot() {
        return BUFFER.toString();
    }
}
