package io.github.hydriostatic.swshrecomp;

import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;
import org.libsdl.app.SDLActivity;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// SDL's activity runs SDL_main of libmain.so (runtime/platform/android.c) on its own thread; the bottom screen of the
// AYN Thor shows the runtime's information panel (DualScreen). Besides: the folder pickers, the game code builder's
// files (genkit), the in-app updater and making the log visible to file managers.
public class MainActivity extends SDLActivity {
    private static final String TAG = "SwShRecomp";
    private static final int PICK_FOLDER = 0x5753;
    private static final String RELEASE_API = "https://api.github.com/repos/Hydriostatic/swsh-recomp-aynthor/releases/tags/thor-latest";
    private static final String ACTION_INSTALLED = "io.github.hydriostatic.swshrecomp.INSTALL_STATUS";
    private DualScreen dual;
    private boolean pickAfterPermission;
    private int pickKind;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable scanLogs = new Runnable() {
        @Override public void run() { scanUserFiles(); ui.postDelayed(this, 10000); }
    };

    static native void nativeFolderPicked(int kind, String path);
    static native void nativeUpdateStatus(String text);

    @Override
    protected String[] getLibraries() { return new String[] {"c++_shared", "SDL3", "main"}; }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        dual = new DualScreen(this);
        new Thread(this::unpackGenkit, "genkit").start();
        // All files access, asked once on the first start: the game files, the built game code and the saves live in
        // SwShRecomp/ and Download/SwShRecomp on shared storage, where they outlast reinstalling the app
        SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        if (!Environment.isExternalStorageManager() && !prefs.getBoolean("asked_files", false)) {
            prefs.edit().putBoolean("asked_files", true).apply();
            askFilesAccess();
        }
        new Thread(() -> checkUpdate(false), "update").start();
        if (getIntent() != null) handleInstallStatus(getIntent());
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        handleInstallStatus(i);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (dual != null) dual.onResume();
        ui.post(scanLogs);
        if (pickAfterPermission) {  // (back from the all-files access screen)
            pickAfterPermission = false;
            if (Environment.isExternalStorageManager()) openPicker();
            else Log.i(TAG, "all-files access not given: no folder picked");
        }
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(scanLogs);
        scanUserFiles();
        if (dual != null) dual.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (dual != null) dual.onDestroy();
        super.onDestroy();
    }

    private void askFilesAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    // ---- the log and saves folder, as file managers see it: files written by path aren't always in Android's media
    // index (which Files and many others list), so they are added to it now and then
    private File userDir() {
        File f = new File(getFilesDir(), "user_dir.txt");
        try {
            if (f.exists()) {
                String d = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
                if (!d.isEmpty()) return new File(d);
            }
        } catch (IOException ignored) {
        }
        return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "SwShRecomp");
    }

    private void scanUserFiles() {
        File[] files = userDir().listFiles();
        if (files == null) return;
        List<String> paths = new ArrayList<>();
        for (File f : files) if (f.isFile()) paths.add(f.getPath());
        if (!paths.isEmpty()) MediaScannerConnection.scanFile(this, paths.toArray(new String[0]), null, null);
    }

    // ---- folders (the start screen: kind 0 = the game folder, 1 = the log and saves folder; runtime/platform/android.c)
    // Native code reads them with plain file calls, which a content:// link from the picker can't give: with all-files
    // access the picked folder is used at its path on storage instead.
    public void pickFolder(int kind) {
        runOnUiThread(() -> {
            pickKind = kind;
            if (Environment.isExternalStorageManager()) { openPicker(); return; }
            pickAfterPermission = true;
            askFilesAccess();
        });
    }

    private void openPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            startActivityForResult(i, PICK_FOLDER);
        } catch (Exception e) {
            Log.e(TAG, "no folder picker: " + e);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FOLDER) { super.onActivityResult(requestCode, resultCode, data); return; }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        String path = treeToPath(data.getData());
        Log.i(TAG, "picked " + data.getData() + " -> " + path);
        if (path != null) nativeFolderPicked(pickKind, path);
    }

    // A folder from the picker (".../tree/primary:Games/SwSh") as its path: primary is internal storage, anything else
    // the volume's id (an SD card: /storage/1234-ABCD).
    static String treeToPath(Uri tree) {
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException e) {
            return null;
        }
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon), rel = id.substring(colon + 1);
        String root = volume.equalsIgnoreCase("primary") ? Environment.getExternalStorageDirectory().getPath()
                                                         : "/storage/" + volume;
        String path = rel.isEmpty() ? root : root + "/" + rel;
        return new File(path).isDirectory() ? path : null;
    }

    // ---- updates: each build of the thor branch is published as the "thor-latest" release (.github/workflows/
    // android-thor.yml) with build.txt (its number) and the APK. Installed over this copy - the same signing key - so
    // the game files and the built game code stay; only a build whose game code id changed asks for a rebuild.
    private static void status(String s) {
        try {
            nativeUpdateStatus(s);
        } catch (UnsatisfiedLinkError ignored) {  // (before libmain is loaded)
        }
    }

    private long ownBuild() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode();
        } catch (Exception e) {
            return 0;
        }
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", "SwShRecomp-Thor");
        c.setRequestProperty("Accept", "application/vnd.github+json, */*");
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int k; (k = in.read(buf)) > 0; ) o.write(buf, 0, k);
        return o.toString("UTF-8");
    }

    public void updateApp(boolean install) {
        new Thread(() -> checkUpdate(install), "update").start();
    }

    private void checkUpdate(boolean install) {
        long own = ownBuild();
        try {
            status("info:Checking for updates... (this is build " + own + ")");
            HttpURLConnection c = open(RELEASE_API);
            if (c.getResponseCode() != 200) { status("info:No update found (build " + own + "; server answered " + c.getResponseCode() + ")"); return; }
            JSONObject rel = new JSONObject(readAll(c.getInputStream()));
            JSONArray assets = rel.getJSONArray("assets");
            String apk = null, buildUrl = null;
            long size = 0;
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String n = a.getString("name");
                if (n.endsWith(".apk")) { apk = a.getString("browser_download_url"); size = a.optLong("size"); }
                if (n.equals("build.txt")) buildUrl = a.getString("browser_download_url");
            }
            if (apk == null || buildUrl == null) { status("info:No update published yet (build " + own + ")"); return; }
            long latest = Long.parseLong(readAll(open(buildUrl).getInputStream()).trim());
            if (latest <= own) { status("info:Up to date (build " + own + ")"); return; }
            if (!install) { status("avail:Build " + latest + " is available (this is build " + own + ")"); return; }
            if (!getPackageManager().canRequestPackageInstalls()) {
                status("avail:Allow \"Install unknown apps\" for SwSh Recomp, then press Install again");
                runOnUiThread(() -> startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                                             Uri.parse("package:" + getPackageName()))));
                return;
            }
            PackageInstaller pi = getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(getPackageName());
            int id = pi.createSession(params);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                HttpURLConnection d = open(apk);
                long total = size > 0 ? size : d.getContentLengthLong(), done = 0;
                int shown = -1;
                try (InputStream in = new BufferedInputStream(d.getInputStream(), 1 << 16);
                     OutputStream out = s.openWrite("SwShRecomp.apk", 0, total > 0 ? total : -1)) {
                    byte[] buf = new byte[1 << 16];
                    for (int k; (k = in.read(buf)) > 0; ) {
                        out.write(buf, 0, k);
                        done += k;
                        int pct = total > 0 ? (int) (done * 100 / total) : -1;
                        if (pct != shown && pct % 5 == 0) { shown = pct; status("info:Downloading build " + latest + "... " + pct + "%"); }
                    }
                    s.fsync(out);
                }
                Intent back = new Intent(this, MainActivity.class).setAction(ACTION_INSTALLED);
                PendingIntent p = PendingIntent.getActivity(this, 0, back, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                status("info:Installing build " + latest + "... (the app restarts)");
                s.commit(p.getIntentSender());
            }
        } catch (Exception e) {
            Log.e(TAG, "update: " + e);
            status((install ? "avail:" : "info:") + "Update failed: " + e.getMessage());
        }
    }

    private void handleInstallStatus(Intent i) {
        if (!ACTION_INSTALLED.equals(i.getAction())) return;
        int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {  // Android asks the player to confirm
            Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) startActivity(confirm);
        } else if (st != PackageInstaller.STATUS_SUCCESS) {
            String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.e(TAG, "install: " + st + " " + msg);
            status("avail:Install failed: " + msg);
        }
    }

    // ---- the game code builder's files (assets/genkit.zip: tools/android/build_genkit.sh) into <files>/res, once per
    // version of the app; runtime/gamebuild.c waits for res/genkit.ok
    private void unpackGenkit() {
        File res = new File(getFilesDir(), "res"), ok = new File(res, "genkit.ok");
        try {
            String want = readVersion(getAssets().open("genkit.zip"));
            if (want == null) { Log.e(TAG, "genkit.zip: no VERSION"); return; }
            if (ok.exists() && want.equals(new String(Files.readAllBytes(ok.toPath()), StandardCharsets.UTF_8))) return;
            ok.delete();
            for (String d : new String[] {"python", "pylib", "tools", "gamelib-src", "zig"}) deleteTree(new File(res, d));
            long t0 = System.currentTimeMillis(), n = 0;
            byte[] buf = new byte[1 << 16];
            try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(getAssets().open("genkit.zip"), 1 << 16))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    File f = new File(res, e.getName());
                    if (!f.getCanonicalPath().startsWith(res.getCanonicalPath() + File.separator)) continue;
                    if (e.isDirectory()) { f.mkdirs(); continue; }
                    f.getParentFile().mkdirs();
                    try (OutputStream o = new FileOutputStream(f)) {
                        for (int k; (k = z.read(buf)) > 0; ) o.write(buf, 0, k);
                    }
                    n++;
                }
            }
            try (OutputStream o = new FileOutputStream(ok)) { o.write(want.getBytes(StandardCharsets.UTF_8)); }
            Log.i(TAG, "genkit: " + n + " files in " + (System.currentTimeMillis() - t0) + " ms");
        } catch (IOException e) {
            Log.e(TAG, "genkit: " + e);
        }
    }

    private static String readVersion(InputStream in) throws IOException {
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(in, 1 << 16))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; )
                if (e.getName().equals("VERSION")) return readAll(z).trim();
        }
        return null;
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
