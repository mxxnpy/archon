// JNI bridge: dev.archon.app.core.NativeClient <-> archon C API.
// Strings cross the boundary as UTF-8 byte arrays (NewStringUTF expects
// "modified UTF-8" and mishandles 4-byte sequences such as emoji).

#include <jni.h>
#include <pthread.h>

#include <cstring>
#include <string>
#include <vector>

#include "archon/archon.h"

namespace {

JavaVM* g_vm = nullptr;
jmethodID g_on_event = nullptr;
pthread_key_t g_detach_key;

void detach_thread(void*) {
    if (g_vm) g_vm->DetachCurrentThread();
}

JNIEnv* env_for_thread() {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    // Detach automatically when the core's network thread exits.
    pthread_setspecific(g_detach_key, env);
    return env;
}

struct Handle {
    arc_client* client = nullptr;
    jobject target = nullptr;  // global ref to the NativeClient
};

jbyteArray bytes(JNIEnv* env, const void* p, size_t n) {
    if (!p) return nullptr;
    jbyteArray a = env->NewByteArray(jsize(n));
    if (a && n) env->SetByteArrayRegion(a, 0, jsize(n), static_cast<const jbyte*>(p));
    return a;
}

jbyteArray str(JNIEnv* env, const char* s) { return s ? bytes(env, s, std::strlen(s)) : nullptr; }

std::string utf8(JNIEnv* env, jbyteArray a) {
    if (!a) return {};
    const jsize n = env->GetArrayLength(a);
    std::string s(size_t(n), '\0');
    env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
    return s;
}

void on_event(void* user, const arc_event* e) {
    auto* h = static_cast<Handle*>(user);
    JNIEnv* env = env_for_thread();
    if (!env) return;
    env->PushLocalFrame(16);
    const jint flags = (e->muted ? 1 : 0) | (e->is_default ? 2 : 0) | (e->playing ? 4 : 0) | (e->system_sounds ? 8 : 0);
    env->CallVoidMethod(h->target, g_on_event, jint(e->type), jint(e->status), jint(e->error), str(env, e->message),
                        str(env, e->id), str(env, e->endpoint_id), str(env, e->name), str(env, e->icon_key),
                        jint(e->pid), jint(e->volume), flags, jint(e->meter), bytes(env, e->data, e->data_len));
    if (env->ExceptionCheck()) env->ExceptionClear();  // never unwind into native code
    env->PopLocalFrame(nullptr);
}

Handle* H(jlong p) { return reinterpret_cast<Handle*>(p); }

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    pthread_key_create(&g_detach_key, detach_thread);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = env->FindClass("dev/archon/app/core/NativeClient");
    if (!cls) return JNI_ERR;
    g_on_event = env->GetMethodID(cls, "onEvent", "(III[B[B[B[B[BIIII[B)V");
    return g_on_event ? JNI_VERSION_1_6 : JNI_ERR;
}

JNIEXPORT jlong JNICALL Java_dev_archon_app_core_NativeClient_nativeCreate(JNIEnv* env, jobject self, jbyteArray id,
                                                                           jbyteArray name) {
    if (!id || env->GetArrayLength(id) != 16) return 0;
    uint8_t raw[16];
    env->GetByteArrayRegion(id, 0, 16, reinterpret_cast<jbyte*>(raw));
    auto* h = new Handle;
    h->target = env->NewGlobalRef(self);
    const std::string n = utf8(env, name);
    h->client = arc_client_create(raw, n.c_str(), on_event, h);
    if (!h->client) {
        env->DeleteGlobalRef(h->target);
        delete h;
        return 0;
    }
    return reinterpret_cast<jlong>(h);
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeDestroy(JNIEnv* env, jobject, jlong p) {
    Handle* h = H(p);
    if (!h) return;
    arc_client_destroy(h->client);  // joins the network thread: no callback after this
    env->DeleteGlobalRef(h->target);
    delete h;
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativePair(JNIEnv* env, jobject, jlong p, jbyteArray host,
                                                                        jint port) {
    if (H(p)) arc_client_pair(H(p)->client, utf8(env, host).c_str(), uint16_t(port));
}

JNIEXPORT jboolean JNICALL Java_dev_archon_app_core_NativeClient_nativeResume(JNIEnv* env, jobject, jlong p,
                                                                              jbyteArray host, jint port,
                                                                              jbyteArray record) {
    if (!H(p) || !record) return JNI_FALSE;
    std::string rec = utf8(env, record);
    const int ok = arc_client_resume(H(p)->client, utf8(env, host).c_str(), uint16_t(port),
                                     reinterpret_cast<const uint8_t*>(rec.data()), rec.size());
    std::fill(rec.begin(), rec.end(), '\0');
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_archon_app_core_NativeClient_nativeResumeMulti(JNIEnv* env, jobject, jlong p,
                                                                                   jobjectArray hosts, jintArray ports,
                                                                                   jbyteArray record) {
    if (!H(p) || !record || !hosts || !ports) return JNI_FALSE;
    const jsize n = env->GetArrayLength(hosts);
    if (n <= 0 || env->GetArrayLength(ports) != n) return JNI_FALSE;
    std::vector<std::string> hs;
    std::vector<const char*> hp;
    std::vector<uint16_t> ps(static_cast<size_t>(n));
    jint* pv = env->GetIntArrayElements(ports, nullptr);
    for (jsize i = 0; i < n; ++i) {
        hs.push_back(utf8(env, static_cast<jbyteArray>(env->GetObjectArrayElement(hosts, i))));
        ps[size_t(i)] = uint16_t(pv[i]);
    }
    env->ReleaseIntArrayElements(ports, pv, JNI_ABORT);
    for (auto& h : hs) hp.push_back(h.c_str());
    std::string rec = utf8(env, record);
    const int ok = arc_client_resume_multi(H(p)->client, hp.data(), ps.data(), size_t(n),
                                           reinterpret_cast<const uint8_t*>(rec.data()), rec.size());
    std::fill(rec.begin(), rec.end(), '\0');
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeSubmitCode(JNIEnv* env, jobject, jlong p,
                                                                              jbyteArray code) {
    if (H(p)) arc_client_submit_code(H(p)->client, utf8(env, code).c_str());
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeDisconnect(JNIEnv*, jobject, jlong p) {
    if (H(p)) arc_client_disconnect(H(p)->client);
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeSetVolume(JNIEnv* env, jobject, jlong p,
                                                                             jboolean endpoint, jbyteArray id,
                                                                             jint volume) {
    if (!H(p)) return;
    const std::string s = utf8(env, id);
    if (endpoint)
        arc_client_set_endpoint_volume(H(p)->client, s.c_str(), uint16_t(volume));
    else
        arc_client_set_source_volume(H(p)->client, s.c_str(), uint16_t(volume));
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeSetMute(JNIEnv* env, jobject, jlong p,
                                                                           jboolean endpoint, jbyteArray id,
                                                                           jboolean muted) {
    if (!H(p)) return;
    const std::string s = utf8(env, id);
    if (endpoint)
        arc_client_set_endpoint_mute(H(p)->client, s.c_str(), muted);
    else
        arc_client_set_source_mute(H(p)->client, s.c_str(), muted);
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeRequestIcon(JNIEnv* env, jobject, jlong p,
                                                                               jbyteArray key) {
    if (H(p)) arc_client_request_icon(H(p)->client, utf8(env, key).c_str());
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeSetMeters(JNIEnv*, jobject, jlong p, jboolean on) {
    if (H(p)) arc_client_set_meters(H(p)->client, on ? 1 : 0);
}

JNIEXPORT void JNICALL Java_dev_archon_app_core_NativeClient_nativeRefresh(JNIEnv*, jobject, jlong p) {
    if (H(p)) arc_client_refresh(H(p)->client);
}

JNIEXPORT jbyteArray JNICALL Java_dev_archon_app_core_NativeClient_nativeGenerateId(JNIEnv* env, jclass) {
    uint8_t id[16];
    arc_generate_controller_id(id);
    return bytes(env, id, 16);
}

// Returns agent id (16 bytes) followed by the UTF-8 PC name, or null.
JNIEXPORT jbyteArray JNICALL Java_dev_archon_app_core_NativeClient_nativeRecordInfo(JNIEnv* env, jclass,
                                                                                    jbyteArray record) {
    if (!record) return nullptr;
    std::string rec = utf8(env, record);
    uint8_t id[16];
    char name[128];
    const int ok = arc_record_info(reinterpret_cast<const uint8_t*>(rec.data()), rec.size(), id, name, sizeof name);
    std::fill(rec.begin(), rec.end(), '\0');
    if (!ok) return nullptr;
    std::string out(reinterpret_cast<const char*>(id), 16);
    out += name;
    return bytes(env, out.data(), out.size());
}

}  // extern "C"
