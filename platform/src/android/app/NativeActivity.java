package android.app;

import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

/**
 * The container's android.app.NativeActivity.
 *
 * The platform class cannot host an armeabi-v7a archive: it looks for a 64-bit
 * library, dlopens it and hands the framework's native glue an activity whose
 * code would have to run as arm64 in this process. This class keeps the same
 * contract towards the archive (onCreate starts the native world, the window
 * arrives later, the lifecycle is forwarded) but the code behind it runs in the
 * guest world under the JIT.
 *
 * It has to carry this name: the runtime verifies that the loader resolves
 * android.app.NativeActivity to the class it is defining, and refuses anything
 * else. The container's guest class loader answers the name with this dex.
 */
public class NativeActivity extends Activity {
    private static final String TAG = "Armbat";

    private SurfaceView surface;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SurfaceView view = new SurfaceView(this);
        surface = view;
        setContentView(view);

        // A native activity consumes its input through AInputQueue; the view it
        // drew on is not the consumer. The touches are taken here, already
        // translated into the archive's own pixels by the container's frame, and
        // queued for the archive's native loop.
        view.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View target, MotionEvent event) {
                touch(event);
                return true;
            }
        });

        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                call("windowCreated", Surface.class, holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                call("windowDestroyed", null, null);
            }
        });

        // The native side starts before the window exists, exactly as in the
        // platform class: the engine sets itself up here and waits for the
        // surface before it draws anything.
        call("create", Surface.class, null);
    }

    @Override
    protected void onStart() {
        super.onStart();
        send(0);
    }

    @Override
    protected void onResume() {
        super.onResume();
        send(1);
    }

    @Override
    protected void onPause() {
        send(3);
        super.onPause();
    }

    @Override
    protected void onStop() {
        send(4);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        send(5);
        surface = null;
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        send(6);
    }

    /**
     * Reaches the container without a compile-time dependency: this dex is
     * built on its own and only meets dev.armbat.GuestNatives at runtime.
     */
    private static Object nativeInstance;

    private static Object natives() throws Exception {
        Object instance = nativeInstance;
        if (instance == null) {
            Class<?> type = Class.forName("dev.armbat.GuestNatives");
            instance = type.getField("INSTANCE").get(null);
            nativeInstance = instance;
        }
        return instance;
    }

    // A touch arrives up to 120 times a second; the lookup cannot repeat.
    private static java.lang.reflect.Method touchMethod;

    private static void touch(MotionEvent event) {
        try {
            java.lang.reflect.Method method = touchMethod;
            if (method == null) {
                Object instance = natives();
                method = instance.getClass().getMethod("touch", MotionEvent.class);
                touchMethod = method;
            }
            method.invoke(natives(), event);
        } catch (Throwable error) {
            Log.e(TAG, "guest touch failed", error);
        }
    }

    private static void send(int event) {
        try {
            Object instance = natives();
            instance.getClass().getMethod("lifecycle", int.class).invoke(instance, event);
        } catch (Throwable error) {
            Log.e(TAG, "guest lifecycle " + event + " failed", error);
        }
    }

    private static void call(String name, Class<?> type, Object argument) {
        try {
            Object instance = natives();
            if (type == null) {
                instance.getClass().getMethod(name).invoke(instance);
            } else {
                instance.getClass().getMethod(name, type).invoke(instance, argument);
            }
        } catch (Throwable error) {
            Log.e(TAG, "guest " + name + " failed", error);
        }
    }
}
