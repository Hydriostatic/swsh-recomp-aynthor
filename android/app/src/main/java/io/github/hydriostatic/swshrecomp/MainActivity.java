package io.github.hydriostatic.swshrecomp;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

// SDL's activity runs SDL_main of libmain.so (runtime/platform/android.c) on its own thread; the bottom screen of the
// AYN Thor shows the runtime's information panel (DualScreen).
public class MainActivity extends SDLActivity {
    private static final String TAG = "SwShRecomp";
    private static final int PICK_FOLDER = 0x5753;
    private DualScreen dual;
    private boolean pickAfterPermission;

    static native void nativeFolderPicked(String path);

    @Override
    protected String[] getLibraries() { return new String[] {"c++_shared", "SDL3", "main"}; }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        dual = new DualScreen(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (dual != null) dual.onResume();
        if (pickAfterPermission) {  // (back from the all-files access screen)
            pickAfterPermission = false;
            if (Environment.isExternalStorageManager()) openPicker();
            else Log.i(TAG, "all-files access not given: no folder picked");
        }
    }

    @Override
    protected void onPause() {
        if (dual != null) dual.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (dual != null) dual.onDestroy();
        super.onDestroy();
    }

    // ---- game folder (the start screen's "Select game folder...", runtime/platform/android.c)
    // Native code reads the game files with plain file calls, which a content:// link from the picker can't give: with
    // all-files access the picked folder is read at its path on storage instead.
    public void pickGameFolder() {
        runOnUiThread(() -> {
            if (Environment.isExternalStorageManager()) { openPicker(); return; }
            pickAfterPermission = true;
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
    }

    private void openPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
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
        if (path != null) nativeFolderPicked(path);
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
}
