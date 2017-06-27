package de.uni_siegen.libqvr;

import android.app.Activity;
import android.content.Context;
import android.view.View;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.os.Bundle;
import android.os.Vibrator;
import android.opengl.GLSurfaceView;
import com.google.vr.ndk.base.AndroidCompat;
import com.google.vr.ndk.base.GvrLayout;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.opengles.GL10;
import org.qtproject.qt5.android.bindings.QtActivity;

public class QVRActivity extends QtActivity
{
    static {
        System.loadLibrary("gvr");
        System.loadLibrary("qvr");
    }

    private EGLContext qvrMasterContext;
    private GvrLayout gvrLayout;
    private GLSurfaceView surfaceView;
    private long nativeGvrContext;

    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        gvrLayout = new GvrLayout(this);
        nativeGvrContext = gvrLayout.getGvrApi().getNativeGvrContext();
    }

    @Override
    protected void onPause() {
        gvrLayout.onPause();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        gvrLayout.onResume();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        gvrLayout.shutdown();
    }

    @Override
    public void onBackPressed() {
        super.onBackPressed();
        gvrLayout.onBackPressed();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // Avoid accidental volume key presses while the phone is in the VR headset.
        if (event.getKeyCode() == KeyEvent.KEYCODE_VOLUME_UP
                || event.getKeyCode() == KeyEvent.KEYCODE_VOLUME_DOWN) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    long getNativeGvrContext()
    {
        return nativeGvrContext;
    }

    // call this function from QVR before initializeVR(), and make sure the master context is current
    // in the currently active thread
    void setMasterContext()
    {
        EGL10 egl = (EGL10)EGLContext.getEGL();
        qvrMasterContext = egl.eglGetCurrentContext();
    }

    // when calling this function from QVR, make sure that it runs on the Android thread
    void initializeVR()
    {
        surfaceView = new GLSurfaceView(this);
        surfaceView.setPreserveEGLContextOnPause(true);
        surfaceView.setEGLConfigChooser(8, 8, 8, 0, 0, 0);
        surfaceView.setEGLContextClientVersion(3);
        surfaceView.setEGLContextFactory(new GLSurfaceView.EGLContextFactory() {
            public EGLContext createContext(EGL10 egl, EGLDisplay display, EGLConfig config) {
                int[] attrib_list = { 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, 3, EGL10.EGL_NONE };
                return egl.eglCreateContext(display, config, qvrMasterContext, attrib_list);
            }
            public void destroyContext(EGL10 egl, EGLDisplay display, EGLContext context) {
                egl.eglDestroyContext(display, context);
            }
        });
        surfaceView.setRenderer(new GLSurfaceView.Renderer() {
            @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) { nativeOnSurfaceCreated(); }
            @Override public void onSurfaceChanged(GL10 gl, int width, int height) {}
            @Override public void onDrawFrame(GL10 gl) { nativeOnDrawFrame(); }
        });
        surfaceView.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent event) {
                if (event.getAction() == MotionEvent.ACTION_DOWN) {
                    ((Vibrator)getSystemService(Context.VIBRATOR_SERVICE)).vibrate(50);
                    surfaceView.queueEvent(new Runnable() {
                        @Override public void run() { nativeOnTriggerEvent(); }
                    });
                    return true;
                }
                return false;
            }
        });
        gvrLayout.setPresentationView(surfaceView);
        setContentView(gvrLayout);
        if (gvrLayout.setAsyncReprojectionEnabled(true)) {
            AndroidCompat.setSustainedPerformanceMode(this, true);
        }
        AndroidCompat.setVrModeEnabled(this, true);
    }

    private native void nativeOnSurfaceCreated();
    private native void nativeOnDrawFrame();
    private native void nativeOnTriggerEvent();
}
