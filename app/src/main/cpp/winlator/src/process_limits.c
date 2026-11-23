/*
 * LURVETA: open-file limit for the guest (Wine + Box64 inherit the app's limits).
 *
 * With WINEESYNC=1 every Windows sync object (event, mutex, semaphore) is an eventfd. GTA V's
 * loader creates enough of them, plus its ~100 RPF archives, to hit the default soft limit of
 * 1024 open files: the game process stayed at 1019 open fds and its loading screen never ended.
 * Raise the soft limit to the hard limit before the guest starts.
 */
#include <jni.h>
#include <stdio.h>
#include <sys/resource.h>

JNIEXPORT jlongArray JNICALL
Java_com_winlator_core_ProcessLimits_raiseOpenFileLimit(JNIEnv* env, jclass clazz) {
    struct rlimit before = {0}, after = {0};
    getrlimit(RLIMIT_NOFILE, &before);
    struct rlimit wanted = before;
    wanted.rlim_cur = before.rlim_max;
    if (wanted.rlim_cur > (rlim_t)1048576) wanted.rlim_cur = 1048576;
    if (wanted.rlim_cur > before.rlim_cur) setrlimit(RLIMIT_NOFILE, &wanted);
    getrlimit(RLIMIT_NOFILE, &after);

    jlong values[3] = {(jlong)before.rlim_cur, (jlong)after.rlim_cur, (jlong)after.rlim_max};
    jlongArray result = (*env)->NewLongArray(env, 3);
    if (result) (*env)->SetLongArrayRegion(env, result, 0, 3, values);
    return result;
}
