/*
 * Host-defined lock callbacks required by projectm-eval's MemoryBuffer.c to
 * guard gmegabuf access across contexts (see api/projectm-eval.h). Our .milk
 * expression evaluation runs once per frame on the main thread only (same
 * threading model as the rest of the modulation matrix in
 * visualizer_manager.cpp), so no locking is needed -- empty functions are
 * explicitly documented as valid for the single-threaded case.
 */
#include "api/projectm-eval.h"

void projectm_eval_memory_host_lock_mutex() {
}

void projectm_eval_memory_host_unlock_mutex() {
}
