# JNI: the native library calls these members by name.
-keep class dev.archon.app.core.NativeClient {
    native <methods>;
    void onEvent(...);
    void onAudio(...);
    void onProfile(...);
}
