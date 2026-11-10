package io.github.hydriostatic.swshrecomp;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The AYN Thor's bottom screen (any second display Android offers for presentations): an information panel drawn
 * from the runtime's values (runtime/panel.h, read through nativeSnapshot a few times a second). It follows the
 * top screen: shown while the activity is in the foreground, gone with it.
 */
final class DualScreen implements DisplayManager.DisplayListener {
    static native String nativeSnapshot();

    private final Activity activity;
    private final DisplayManager dm;
    private Panel panel;
    private boolean resumed;

    DualScreen(Activity a) {
        activity = a;
        dm = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        dm.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
    }

    void onResume() { resumed = true; show(); }
    void onPause() { resumed = false; hide(); }
    void onDestroy() { dm.unregisterDisplayListener(this); hide(); }

    private Display secondDisplay() {
        Display own = activity.getWindowManager().getDefaultDisplay();
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION))
            if (d.getDisplayId() != own.getDisplayId()) return d;
        return null;
    }

    private void show() {
        if (!resumed || panel != null) return;
        Display d = secondDisplay();
        if (d == null) return;
        try {
            panel = new Panel(activity, d);
            panel.show();
        } catch (WindowManager.InvalidDisplayException e) {
            panel = null;
        }
    }

    private void hide() {
        if (panel != null) { panel.dismiss(); panel = null; }
    }

    @Override public void onDisplayAdded(int id) { show(); }
    @Override public void onDisplayChanged(int id) {}
    @Override public void onDisplayRemoved(int id) {
        if (panel != null && panel.getDisplay().getDisplayId() == id) hide();
    }

    private static final class Panel extends Presentation {
        Panel(Context c, Display d) { super(c, d); }

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            setContentView(new PanelView(getContext()));
        }
    }

    /** Draws the sections: a header with the state, the frame rate large, then one card per section. */
    private static final class PanelView extends View {
        private static final int BG = Color.rgb(0x30, 0x30, 0x30);        // (empty screen: the menus' grey)
        private static final int CARD = Color.rgb(0x3c, 0x3c, 0x3c);
        private static final int HEADER = Color.rgb(0x22, 0x22, 0x22);
        private static final int TEXT = Color.rgb(0xee, 0xee, 0xee);
        private static final int DIM = Color.rgb(0x9a, 0x9a, 0x9a);
        private static final int GOOD = Color.rgb(0x5c, 0xd6, 0x7c), WARN = Color.rgb(0xf2, 0xc1, 0x4e), BAD = Color.rgb(0xf0, 0x5d, 0x5d);
        private static final int ACCENT = Color.rgb(0x3d, 0x8b, 0xff);   // (Sword blue)

        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Handler h = new Handler(Looper.getMainLooper());
        private final Runnable tick = new Runnable() {
            @Override public void run() { invalidate(); h.postDelayed(this, 250); }
        };
        private final IntentFilter battery = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        private final RectF r = new RectF();

        PanelView(Context c) { super(c); }

        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); h.post(tick); }
        @Override protected void onDetachedFromWindow() { h.removeCallbacks(tick); super.onDetachedFromWindow(); }

        private Map<String, List<String[]>> read() {
            Map<String, List<String[]>> m = new LinkedHashMap<>();
            String text;
            try {
                text = nativeSnapshot();
            } catch (UnsatisfiedLinkError e) {
                text = "status\tstate\tLoading\n";
            }
            for (String line : text.split("\n")) {
                String[] f = line.split("\t", 3);
                if (f.length < 3) continue;
                List<String[]> l = m.get(f[0]);
                if (l == null) m.put(f[0], l = new ArrayList<>());
                l.add(new String[] {f[1], f[2]});
            }
            Intent bi = getContext().registerReceiver(null, battery);  // (sticky: no receiver kept)
            if (bi != null) {
                int lv = bi.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), sc = bi.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                int temp = bi.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
                int plug = bi.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
                List<String[]> l = m.get("system");
                if (l == null) m.put("system", l = new ArrayList<>());
                if (lv >= 0) l.add(new String[] {"battery", (lv * 100 / Math.max(sc, 1)) + "%" + (plug != 0 ? " (charging)" : "")});
                if (temp != Integer.MIN_VALUE) l.add(new String[] {"battery temp", String.format("%.1f °C", temp / 10f)});
            }
            return m;
        }

        private static String value(Map<String, List<String[]>> m, String sec, String key) {
            List<String[]> l = m.get(sec);
            if (l != null) for (String[] kv : l) if (kv[0].equals(key)) return kv[1];
            return null;
        }

        @Override protected void onDraw(Canvas c) {
            float s = getWidth() / 1080f, pad = 40 * s;
            c.drawColor(BG);
            Map<String, List<String[]>> m = read();

            // header: app name, game, state
            p.setColor(HEADER);
            c.drawRect(0, 0, getWidth(), 170 * s, p);
            p.setTypeface(Typeface.DEFAULT_BOLD);
            p.setTextSize(52 * s);
            p.setColor(TEXT);
            String game = value(m, "status", "game");
            c.drawText(game != null ? game : "SwSh Recomp", pad, 80 * s, p);
            p.setTypeface(Typeface.DEFAULT);
            p.setTextSize(34 * s);
            p.setColor(DIM);
            String ver = value(m, "status", "version"), state = value(m, "status", "state");
            c.drawText((ver != null ? "v" + ver + "  ·  " : "") + (state != null ? state : ""), pad, 135 * s, p);
            float y = 170 * s + pad;

            // the frame rate, large, coloured against the target
            String fps = value(m, "performance", "fps");
            if (fps != null) {
                float f = parse(fps), target = parse(value(m, "performance", "target"));
                p.setColor(target <= 0 || f >= target * 0.95f ? GOOD : f >= target * 0.75f ? WARN : BAD);
                p.setTypeface(Typeface.DEFAULT_BOLD);
                p.setTextSize(200 * s);
                c.drawText(fps, pad, y + 180 * s, p);
                float w = p.measureText(fps);
                p.setTypeface(Typeface.DEFAULT);
                p.setTextSize(48 * s);
                p.setColor(DIM);
                c.drawText("fps" + (target > 0 ? " / " + (int) target : ""), pad + w + 24 * s, y + 180 * s, p);
                String ms = value(m, "performance", "frame time");
                if (ms != null) c.drawText(ms, pad + w + 24 * s, y + 110 * s, p);
                y += 230 * s;
            }

            // one card per section (status and the frame rate are drawn above)
            for (Map.Entry<String, List<String[]>> e : m.entrySet()) {
                List<String[]> rows = new ArrayList<>();
                for (String[] kv : e.getValue()) {
                    if (e.getKey().equals("status")) continue;
                    if (e.getKey().equals("performance") && (kv[0].equals("fps") || kv[0].equals("target") || kv[0].equals("frame time"))) continue;
                    rows.add(kv);
                }
                if (rows.isEmpty()) continue;
                float rowH = 64 * s, top = y, bottom = y + 80 * s + rows.size() * rowH;
                if (bottom > getHeight() - pad) break;
                r.set(pad, top, getWidth() - pad, bottom);
                p.setColor(CARD);
                c.drawRoundRect(r, 24 * s, 24 * s, p);
                p.setColor(ACCENT);
                p.setTypeface(Typeface.DEFAULT_BOLD);
                p.setTextSize(30 * s);
                c.drawText(e.getKey().toUpperCase(), pad * 2, top + 56 * s, p);
                float ry = top + 80 * s;
                for (String[] kv : rows) {
                    p.setTypeface(Typeface.DEFAULT);
                    p.setTextSize(38 * s);
                    p.setColor(DIM);
                    c.drawText(kv[0], pad * 2, ry + 46 * s, p);
                    p.setColor(TEXT);
                    float vw = p.measureText(kv[1]);
                    c.drawText(kv[1], getWidth() - pad * 2 - vw, ry + 46 * s, p);
                    ry += rowH;
                }
                y = bottom + pad * 0.75f;
            }
        }

        private static float parse(String v) {
            if (v == null) return 0;
            try { return Float.parseFloat(v.replaceAll("[^0-9.]", "")); } catch (NumberFormatException e) { return 0; }
        }
    }
}
