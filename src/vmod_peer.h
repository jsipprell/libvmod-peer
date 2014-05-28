#ifndef VMOD_PEER_H
#define VMOD_PEER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cache/cache.h"
#include "vrt.h"
#include "vsb.h"
#include "vsa.h"

/* FIXME: vrt.h not included in varnish4 header files */
#include "vct.h"

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

/* Varnish 4 interface */
struct vmod_peer;
struct vmod_peer_ip {
  unsigned magic;
#define VMOD_PEER_IP_MAGIC 0xa05b179c
  const char *name;
  struct suckaddr *addr;
  struct vmod_peer *vp;
};

#define V4_VMOD_PEER(sym,name,...) \
vmod_##sym##_##name(const struct vrt_ctx *ctx, \
                    struct vmod_peer_##sym *vmp, ## __VA_ARGS__)

#endif /* VMOD_PEER_H */
