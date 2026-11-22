#ifndef VORTEK_CRASH_HANDLER_H
#define VORTEK_CRASH_HANDLER_H

/* LURVETA: records native crashes of the app process (Vortek server, texture decoder, ...) to
 * files/logs/native-crash.txt with a symbolised backtrace, then lets Android's crash handling run
 * as before. The Java side copies the file next to the launch report on the next start. */

void CrashHandler_install(void);
/* Gives the calling thread an alternate signal stack, so a stack overflow can still be reported. */
void CrashHandler_threadInit(void);
/* What the calling thread is doing, printed if it crashes. */
void CrashHandler_setActivity(int requestCode, int clientPid);
/* How request codes are named in the report. */
void CrashHandler_setRequestNames(const char* (*lookup)(short));

#endif
