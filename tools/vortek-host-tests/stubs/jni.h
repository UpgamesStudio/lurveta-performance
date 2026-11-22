/* Host-test stub: only the JNI types referenced by Vortek headers. */
#ifndef HOST_STUB_JNI_H
#define HOST_STUB_JNI_H
#include <stdint.h>
typedef void* jobject;
typedef void* jclass;
typedef void* jstring;
typedef void* jobjectArray;
typedef void* jmethodID;
typedef void* jfieldID;
typedef int32_t jint;
typedef int64_t jlong;
typedef uint8_t jboolean;
typedef struct JavaVM_* JavaVM;
typedef struct JNIEnv_* JNIEnv;
#endif
