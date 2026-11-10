package io.github.hydriostatic.swshrecomp;

import org.libsdl.app.SDLActivity;

// SDL's activity runs SDL_main of libmain.so (runtime/platform/android.c) on its own thread.
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() { return new String[] {"c++_shared", "SDL3", "main"}; }
}
