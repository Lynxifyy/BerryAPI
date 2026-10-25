// Android WifiManager multicast lock. See multicast_lock.h.
//
// The lock is acquired through JNI so no Java code or Gradle change is needed:
// the injected library resolves the running app's JavaVM, gets the application
// Context from ActivityThread, and asks its WifiManager for a multicast lock.
// Every JNI type is kept inside this translation unit, so no caller of the
// header needs jni.h. On a non-Android build the whole body compiles out and
// the seam reports unsupported.

#include "multicast_lock.h"

#include "berry/logger.h"

#if defined(BERRY_PLATFORM_ANDROID) && !defined(BERRY_MULTICAST_LOCK_NULL)

#include <dlfcn.h>
#include <jni.h>

#include <mutex>

namespace Berry {
namespace {

/// Resolves the process's JavaVM. A plain app exports `JNI_GetCreatedJavaVMs`
/// through libart (reachable via RTLD_DEFAULT once the VM is up); a build that
/// hides it still finds it by opening libart directly.
JavaVM* ResolveJavaVm() {
  using GetVmsFn = jint (*)(JavaVM**, jsize, jsize*);
  GetVmsFn getVms = reinterpret_cast<GetVmsFn>(dlsym(RTLD_DEFAULT,
                                                     "JNI_GetCreatedJavaVMs"));
  if (getVms == nullptr) {
    void* art = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
    if (art != nullptr) {
      getVms = reinterpret_cast<GetVmsFn>(dlsym(art, "JNI_GetCreatedJavaVMs"));
    }
  }
  if (getVms == nullptr) return nullptr;
  JavaVM* vm = nullptr;
  jsize count = 0;
  if (getVms(&vm, 1, &count) != JNI_OK || count < 1) return nullptr;
  return vm;
}

/// A scoped JNIEnv that detaches only if it attached the thread.
struct ScopedEnv {
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  bool attached = false;

  bool Open(JavaVM* v) {
    vm = v;
    if (vm == nullptr) return false;
    const jint r = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (r == JNI_OK) return true;
    if (r != JNI_EDETACHED) return false;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    attached = true;
    return true;
  }
  ~ScopedEnv() {
    if (attached && vm != nullptr) vm->DetachCurrentThread();
  }
};

/// The application Context, or null. Goes through ActivityThread, which every
/// Android app process has, rather than requiring a caller to pass one in.
jobject CurrentApplication(JNIEnv* env) {
  jclass at = env->FindClass("android.app.ActivityThread");
  if (at == nullptr) return nullptr;
  jmethodID cur = env->GetStaticMethodID(
      at, "currentApplication", "()Landroid/app/Application;");
  if (cur == nullptr) return nullptr;
  jobject app = env->CallStaticObjectMethod(at, cur);
  return app;  // may be null if no application is up yet
}

}  // namespace

struct MulticastLock::Impl {
  JavaVM* vm = nullptr;
  jobject lock = nullptr;  // global ref to the WifiManager.MulticastLock
};

MulticastLock::~MulticastLock() {
  Release();
  delete impl_;
  impl_ = nullptr;
}

bool MulticastLock::available() { return true; }

bool MulticastLock::Acquire(const std::string& tag) {
  if (held_) return true;  // idempotent
  JavaVM* vm = ResolveJavaVm();
  if (vm == nullptr) {
    err_ = "no JavaVM (multicast lock unsupported)";
    return false;
  }
  ScopedEnv se;
  if (!se.Open(vm)) {
    err_ = "cannot attach to the JVM";
    return false;
  }
  JNIEnv* env = se.env;
  bool ok = false;
  // A local frame keeps every temporary reference bounded and releases them
  // even on an early return.
  if (env->PushLocalFrame(16) == 0) {
    jobject app = CurrentApplication(env);
    if (app != nullptr) {
      jclass ctxCls = env->GetObjectClass(app);
      jmethodID getSvc = env->GetMethodID(
          ctxCls, "getSystemService",
          "(Ljava/lang/String;)Ljava/lang/Object;");
      jstring wifiName = env->NewStringUTF("wifi");
      jobject wifi = getSvc != nullptr
                         ? env->CallObjectMethod(app, getSvc, wifiName)
                         : nullptr;
      if (wifi != nullptr) {
        jclass wmCls = env->GetObjectClass(wifi);
        jmethodID create = env->GetMethodID(
            wmCls, "createMulticastLock",
            "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
        jstring jtag = env->NewStringUTF(tag.c_str());
        jobject lock = create != nullptr
                           ? env->CallObjectMethod(wifi, create, jtag)
                           : nullptr;
        if (lock != nullptr) {
          jclass lockCls = env->GetObjectClass(lock);
          jmethodID acquire = env->GetMethodID(lockCls, "acquire", "()V");
          if (acquire != nullptr) {
            env->CallVoidMethod(lock, acquire);
            if (env->ExceptionCheck()) {
              env->ExceptionClear();
            } else {
              if (impl_ == nullptr) impl_ = new Impl();
              impl_->vm = vm;
              impl_->lock = env->NewGlobalRef(lock);
              ok = impl_->lock != nullptr;
            }
          }
        }
      }
    }
    env->PopLocalFrame(nullptr);
  }
  if (!ok) {
    err_ = "could not acquire the Wi-Fi multicast lock";
    Log::Warn("Berry: multicast lock unavailable (%s)", err_.c_str());
    return false;
  }
  held_ = true;
  usable_ = true;
  err_.clear();
  Log::Info("Berry: Wi-Fi multicast lock acquired (%s)", tag.c_str());
  return true;
}

void MulticastLock::Release() {
  if (!held_) return;
  held_ = false;
  if (impl_ != nullptr && impl_->lock != nullptr) {
    ScopedEnv se;
    if (se.Open(impl_->vm)) {
      jclass lockCls = se.env->GetObjectClass(impl_->lock);
      jmethodID release = se.env->GetMethodID(lockCls, "release", "()V");
      if (release != nullptr) {
        se.env->CallVoidMethod(impl_->lock, release);
        if (se.env->ExceptionCheck()) se.env->ExceptionClear();
      }
      se.env->DeleteGlobalRef(impl_->lock);
    }
    impl_->lock = nullptr;
  }
  usable_ = false;
}

}  // namespace Berry

#else  // non-Android / explicitly disabled: the seam is a fail-closed no-op.

namespace Berry {

struct MulticastLock::Impl {};

MulticastLock::~MulticastLock() {
  delete impl_;
  impl_ = nullptr;
}

bool MulticastLock::available() { return false; }

bool MulticastLock::Acquire(const std::string&) {
  // Not an error: the host has no Wi-Fi power management to defeat.
  err_ = "multicast lock not supported on this platform";
  return false;
}

void MulticastLock::Release() { held_ = false; }

}  // namespace Berry

#endif
