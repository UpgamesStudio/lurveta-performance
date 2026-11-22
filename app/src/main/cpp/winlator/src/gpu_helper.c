#include <jni.h>
#include <malloc.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <EGL/egl.h>

#include "winlator.h"
#include "file_utils.h"

EGLContext globalEGLContext = EGL_NO_CONTEXT;

JNIEXPORT jobjectArray JNICALL
Java_com_winlator_core_GPUHelper_vkGetDeviceExtensions(JNIEnv *env, jclass obj) {
    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkExtensionProperties* properties = NULL;
    jobjectArray stringArray = NULL;
    VkResult result;
    VkInstance instance = VK_NULL_HANDLE;

    void* libvulkan = dlopen(LIBVULKAN_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) goto done;

    PFN_vkCreateInstance vkCreateInstance = dlsym(libvulkan, "vkCreateInstance");
    PFN_vkDestroyInstance vkDestroyInstance = dlsym(libvulkan, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = dlsym(libvulkan, "vkEnumeratePhysicalDevices");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = dlsym(libvulkan, "vkEnumerateDeviceExtensionProperties");

    result = vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) goto done;

    uint32_t deviceCount = 1;
    VkPhysicalDevice physicalDevice;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, &physicalDevice);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) goto done;

    uint32_t propertyCount = 0;
    result = vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &propertyCount, NULL);
    if (result != VK_SUCCESS || propertyCount == 0) goto done;

    properties = calloc(propertyCount, sizeof(VkExtensionProperties));
    result = vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &propertyCount, properties);
    if (result != VK_SUCCESS) goto done;

    stringArray = (*env)->NewObjectArray(env, propertyCount, (*env)->FindClass(env, "java/lang/String"), 0);
    for (int i = 0; i < propertyCount; i++) {
        jstring string = (*env)->NewStringUTF(env, properties[i].extensionName);
        (*env)->SetObjectArrayElement(env, stringArray, i, string);
    }

done:
    if (instance) vkDestroyInstance(instance, NULL);
    if (properties) free(properties);
    if (!stringArray) stringArray = (*env)->NewObjectArray(env, 0, (*env)->FindClass(env, "java/lang/String"), 0);
    if (libvulkan) dlclose(libvulkan);
    return stringArray;
}

JNIEXPORT jint JNICALL
Java_com_winlator_core_GPUHelper_vkGetApiVersion() {
    int version = 0;
    char* content = fileGetContents(APP_CACHE_DIR "/.vk-api-version", NULL, NULL);
    if (content) {
        version = strtol(content, NULL, 10);
        MEMFREE(content);
        if (version > 0) return version;
    }

    VkResult result;
    VkInstance instance = VK_NULL_HANDLE;

    void* libvulkan = dlopen(LIBVULKAN_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) goto done;

    PFN_vkCreateInstance vkCreateInstance = dlsym(libvulkan, "vkCreateInstance");
    PFN_vkDestroyInstance vkDestroyInstance = dlsym(libvulkan, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = dlsym(libvulkan, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = dlsym(libvulkan, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = dlsym(libvulkan, "vkGetPhysicalDeviceFeatures");

    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    result = vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) goto done;

    uint32_t deviceCount = 1;
    VkPhysicalDevice physicalDevice;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, &physicalDevice);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) goto done;

    VkPhysicalDeviceProperties properties = {0};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    version = properties.apiVersion;
    if (version >= VK_MAKE_VERSION(1, 3, 0)) {
        VkPhysicalDeviceFeatures features = {0};
        vkGetPhysicalDeviceFeatures(physicalDevice, &features);
        VkPhysicalDeviceFeatures requiredFeatures = {VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE, VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE, VK_TRUE, VK_TRUE, VK_TRUE, VK_TRUE, VK_FALSE, VK_TRUE, VK_TRUE};

        for (int offset = 0; offset < sizeof(VkPhysicalDeviceFeatures); offset += sizeof(VkBool32)) {
            VkBool32 srcValue = *(VkBool32*)(((char*)&features) + offset);
            VkBool32 dstValue = *(VkBool32*)(((char*)&requiredFeatures) + offset);

            if (srcValue != dstValue) {
                version = VK_MAKE_VERSION(1, 2, 0);
                break;
            }
        }
    }

    char value[32] = {0};
    sprintf(value, "%d", version);
    filePutContents(APP_CACHE_DIR "/.vk-api-version", value, strlen(value));

done:
    if (instance) vkDestroyInstance(instance, NULL);
    if (libvulkan) dlclose(libvulkan);
    return version;
}

/*
 * LURVETA: Vulkan device identity used for GPU detection (ARM Mali / Immortalis, Qualcomm Adreno,
 * Imagination PowerVR, Samsung Xclipse, ...). Returns "key=value" strings; empty array on failure.
 */
JNIEXPORT jobjectArray JNICALL
Java_com_winlator_core_GPUHelper_vkGetDeviceInfo(JNIEnv *env, jclass obj) {
    char lines[12][288];
    int lineCount = 0;
    VkInstance instance = VK_NULL_HANDLE;

    void* libvulkan = dlopen(LIBVULKAN_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) goto done;

    PFN_vkCreateInstance vkCreateInstance = dlsym(libvulkan, "vkCreateInstance");
    PFN_vkDestroyInstance vkDestroyInstance = dlsym(libvulkan, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = dlsym(libvulkan, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = dlsym(libvulkan, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = dlsym(libvulkan, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = dlsym(libvulkan, "vkGetPhysicalDeviceFeatures");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties) goto done;

    VkApplicationInfo appInfo = {0};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    if (vkCreateInstance(&createInfo, NULL, &instance) != VK_SUCCESS) goto done;

    uint32_t deviceCount = 1;
    VkPhysicalDevice physicalDevice;
    VkResult result = vkEnumeratePhysicalDevices(instance, &deviceCount, &physicalDevice);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || deviceCount == 0) goto done;

    VkPhysicalDeviceProperties properties = {0};
    VkPhysicalDeviceDriverProperties driverProperties = {0};
    driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    if (vkGetPhysicalDeviceProperties2 && properties.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceProperties2 properties2 = {0};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driverProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    }

    VkPhysicalDeviceFeatures features = {0};
    if (vkGetPhysicalDeviceFeatures) vkGetPhysicalDeviceFeatures(physicalDevice, &features);

    snprintf(lines[lineCount++], sizeof(lines[0]), "vendorID=%u", properties.vendorID);
    snprintf(lines[lineCount++], sizeof(lines[0]), "deviceID=%u", properties.deviceID);
    snprintf(lines[lineCount++], sizeof(lines[0]), "deviceName=%s", properties.deviceName);
    snprintf(lines[lineCount++], sizeof(lines[0]), "apiVersion=%u", properties.apiVersion);
    snprintf(lines[lineCount++], sizeof(lines[0]), "driverVersion=%u", properties.driverVersion);
    snprintf(lines[lineCount++], sizeof(lines[0]), "deviceType=%d", properties.deviceType);
    snprintf(lines[lineCount++], sizeof(lines[0]), "driverID=%d", driverProperties.driverID);
    snprintf(lines[lineCount++], sizeof(lines[0]), "driverName=%s", driverProperties.driverName);
    snprintf(lines[lineCount++], sizeof(lines[0]), "driverInfo=%s", driverProperties.driverInfo);
    snprintf(lines[lineCount++], sizeof(lines[0]), "textureCompressionBC=%d", features.textureCompressionBC);
    snprintf(lines[lineCount++], sizeof(lines[0]), "textureCompressionETC2=%d", features.textureCompressionETC2);
    snprintf(lines[lineCount++], sizeof(lines[0]), "textureCompressionASTC_LDR=%d", features.textureCompressionASTC_LDR);

done:
    if (instance) {
        PFN_vkDestroyInstance destroyInstance = libvulkan ? dlsym(libvulkan, "vkDestroyInstance") : NULL;
        if (destroyInstance) destroyInstance(instance, NULL);
    }
    if (libvulkan) dlclose(libvulkan);

    jobjectArray stringArray = (*env)->NewObjectArray(env, lineCount, (*env)->FindClass(env, "java/lang/String"), 0);
    for (int i = 0; i < lineCount; i++) {
        jstring string = (*env)->NewStringUTF(env, lines[i]);
        (*env)->SetObjectArrayElement(env, stringArray, i, string);
        (*env)->DeleteLocalRef(env, string);
    }
    return stringArray;
}

JNIEXPORT void JNICALL
Java_com_winlator_core_GPUHelper_setGlobalEGLContext(JNIEnv *env, jclass obj) {
    globalEGLContext = eglGetCurrentContext();
}

JNIEXPORT jlong JNICALL
Java_com_winlator_core_GPUHelper_createOffscreenEGLContext(JNIEnv *env, jclass obj,
                                                           jboolean sharedContext) {
    static const EGLint confAttribList[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE,
    };
    static const EGLint ctxAttribList[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLBoolean success;

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglDisplay) return 0;

    EGLint major, minor;
    success = eglInitialize(eglDisplay, &major, &minor);
    if (!success) return 0;

    int numConfigs;
    EGLConfig eglConfig;
    success = eglChooseConfig(eglDisplay, confAttribList, &eglConfig, 1, &numConfigs);
    if (!success || numConfigs != 1) return 0;

    EGLContext eglContext = eglCreateContext(eglDisplay, eglConfig, sharedContext ? globalEGLContext : NULL, ctxAttribList);

    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext);
    return (jlong)eglContext;
}

JNIEXPORT void JNICALL
Java_com_winlator_core_GPUHelper_destroyOffscreenEGLContext(JNIEnv *env, jclass obj,
                                                            jlong contextPtr) {
    if (contextPtr == 0) return;
    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(eglDisplay, (EGLContext)contextPtr);
}