#ifndef VORTEK_TASK_GROUP_H
#define VORTEK_TASK_GROUP_H

/*
 * LURVETA Performance — wait for a specific set of thread-pool tasks.
 *
 * ThreadPool_wait() waits until the WHOLE pool is idle. Vortek used one pool for both BC texture
 * decoding and asynchronous pipeline compilation, so every multi-threaded texture decode (done
 * while the game's vkQueueSubmit is blocked) also waited for all in-flight shader compilations.
 * A TaskGroup only waits for the tasks that were added to it.
 */

#include <pthread.h>
#include <stdbool.h>

typedef struct TaskGroup {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int pending;
} TaskGroup;

static inline void TaskGroup_init(TaskGroup* group) {
    pthread_mutex_init(&group->mutex, NULL);
    pthread_cond_init(&group->cond, NULL);
    group->pending = 0;
}

static inline void TaskGroup_destroy(TaskGroup* group) {
    pthread_cond_destroy(&group->cond);
    pthread_mutex_destroy(&group->mutex);
}

static inline void TaskGroup_add(TaskGroup* group, int count) {
    pthread_mutex_lock(&group->mutex);
    group->pending += count;
    pthread_mutex_unlock(&group->mutex);
}

static inline void TaskGroup_done(TaskGroup* group) {
    pthread_mutex_lock(&group->mutex);
    if (--group->pending <= 0) {
        group->pending = 0;
        pthread_cond_broadcast(&group->cond);
    }
    pthread_mutex_unlock(&group->mutex);
}

static inline void TaskGroup_wait(TaskGroup* group) {
    pthread_mutex_lock(&group->mutex);
    while (group->pending > 0) pthread_cond_wait(&group->cond, &group->mutex);
    pthread_mutex_unlock(&group->mutex);
}

#endif
