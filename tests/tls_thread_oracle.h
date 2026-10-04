#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*getter_t)(void);
static getter_t get_counter, get_zero, get_bytes, get_offset, get_alias;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned arrivals, generation;
static void barrier(void) {
    pthread_mutex_lock(&mutex);
    unsigned current = generation;
    if (++arrivals == 5) {
        arrivals = 0;
        generation++;
        pthread_cond_broadcast(&condition);
    } else {
        while (generation == current)
            pthread_cond_wait(&condition, &mutex);
    }
    pthread_mutex_unlock(&mutex);
}

typedef struct worker {
    int id, failures;
    int64_t *counter;
    unsigned char *zero, *bytes;
} worker_t;

static void *run_worker(void *argument) {
    worker_t *worker = argument;
    worker->counter = get_counter();
    worker->zero = get_zero();
    worker->bytes = get_bytes();
    worker->failures += *worker->counter != 17;
    worker->failures += get_alias && get_alias() != worker->counter;
    worker->failures += ((uintptr_t)worker->counter % 64) != 0;
    worker->failures += ((uintptr_t)worker->zero % 128) != 0;
    worker->failures += memcmp(worker->bytes, "ABCD", 4) != 0;
    worker->failures += get_offset() != worker->bytes + 2;
    for (unsigned i = 0; i < 32; i++)
        worker->failures += worker->zero[i] != 0;
    barrier();
    *worker->counter = 100 + worker->id;
    worker->zero[31] = (unsigned char)(70 + worker->id);
    worker->bytes[0] = (unsigned char)('a' + worker->id);
    barrier();
    worker->failures += *worker->counter != 100 + worker->id;
    worker->failures += worker->zero[31] != 70 + worker->id;
    worker->failures += worker->bytes[0] != 'a' + worker->id;
    worker->failures += get_counter() != worker->counter;
    barrier();
    return NULL;
}

static int check_tls_storage(void) {
    int64_t *main_counter = get_counter();
    unsigned char *main_zero = get_zero(), *main_bytes = get_bytes();
    if (!main_counter || !main_zero || !main_bytes) return 1;
    if (*main_counter != 17 || memcmp(main_bytes, "ABCD", 4) != 0) {
        fprintf(stderr, "TLS initial values: counter=%lld bytes=%02x%02x%02x%02x\n",
            (long long)*main_counter, main_bytes[0], main_bytes[1], main_bytes[2], main_bytes[3]);
        return 1;
    }
    if (get_alias && get_alias() != main_counter) return 1;
    *main_counter = -99;
    main_zero[31] = 33;
    main_bytes[0] = 'Z';
    int failures = 0;
    /* A second wave verifies fresh initialization after thread teardown. */
    for (unsigned wave = 0; wave < 2; wave++) {
        pthread_t threads[4];
        worker_t workers[4] = {{0}};
        for (int i = 0; i < 4; i++) {
            workers[i].id = i;
            if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0)
                abort();
        }
        barrier();
        for (unsigned i = 0; i < 4; i++) {
            failures += workers[i].counter == main_counter;
            for (unsigned j = 0; j < i; j++)
                failures += workers[i].counter == workers[j].counter;
        }
        barrier();
        failures += *main_counter != -99 || main_zero[31] != 33 || main_bytes[0] != 'Z';
        barrier();
        for (unsigned i = 0; i < 4; i++) {
            pthread_join(threads[i], NULL);
            failures += workers[i].failures;
        }
    }
    if (failures) fprintf(stderr, "TLS behavioral failures: %d\n", failures);
    else puts("PASS TLS: distinct live addresses, initialization, alignment, offsets, persistence and fresh threads");
    return failures != 0;
}
