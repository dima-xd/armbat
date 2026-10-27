package android.view;

import android.content.Context;

/**
 * Compile-time stand-in for the platform's WindowManagerImpl.
 *
 * The class is public at runtime but absent from android.jar, and the guest's
 * window manager has to inherit from it: the platform casts a WindowManager to
 * WindowManagerImpl inside Window.setWindowManager, which is where Dialog takes
 * its window from. This declaration is only seen by the compiler -- it is a
 * compileOnly class path entry and never ends up in the APK; the runtime uses
 * the platform's own class.
 *
 * Only what the container's subclass names is declared. Every other
 * WindowManager call is delegated to the container's manager by the subclass,
 * so the platform implementation of them is what runs anyway.
 */
public class WindowManagerImpl {
    public WindowManagerImpl(Context context) {
    }

    public Display getDefaultDisplay() {
        return null;
    }
}
