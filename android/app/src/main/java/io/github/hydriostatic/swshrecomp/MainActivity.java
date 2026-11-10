package io.github.hydriostatic.swshrecomp;

import android.os.Bundle;

import org.libsdl.app.SDLActivity;

// SDL's activity runs SDL_main of libmain.so (runtime/platform/android.c) on its own thread; the bottom screen of the
// AYN Thor shows the runtime's information panel (DualScreen).
public class MainActivity extends SDLActivity {
    private DualScreen dual;

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
}
