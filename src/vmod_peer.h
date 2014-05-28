#ifndef VMOD_PEER_H
#define VMOD_PEER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vrt.h"
#include "vsb.h"
#include "vct.h"
#include "bin/varnishd/cache.h"

#include "vcc_if.h"

#ifndef VMOD_THREAD_FUNC
#define VMOD_THREAD_FUNC void *
#endif

#ifndef VMOD_PEER_MAX_QUEUE_DEPTH
/* maximum number of http requests allowed to be queued up before discarding new ones */
#define VMOD_PEER_MAX_QUEUE_DEPTH 50
#endif

typedef struct vp_lock vp_lock_t;

extern vp_lock_t *vp_lock_new(pthread_mutexattr_t*,pthread_condattr_t*);
extern void vp_lock_destroy(vp_lock_t*);
extern int vp_lock_read_acquire(vp_lock_t*);
extern int vp_lock_write_acquire(vp_lock_t*);
extern int vp_lock_release(vp_lock_t*);

#endif /* VMOD_PEER_H */
