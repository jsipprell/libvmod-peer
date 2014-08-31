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

#ifndef VMOD_PEER_MAX_THREAD_POOL
/* maximum number of threads allowed in the worker thread pool, this will start at the
 * min setting (1 by default) and grow to the configured max (again, 1 by default) but
 * the configured max may never exceed this limit.
 */
#define VMOD_PEER_MAX_THREAD_POOL 100
#endif

typedef struct vp_lock vp_lock_t;

extern vp_lock_t *vp_lock_new(pthread_mutexattr_t*,pthread_condattr_t*);
extern void vp_lock_destroy(vp_lock_t*);
extern int vp_lock_read_acquire(vp_lock_t*);
extern int vp_lock_write_acquire(vp_lock_t*);
extern int vp_lock_release(vp_lock_t*);

/* mutex wrappers ala varnish's locks (they utilize varnish's private lock structure
 * which is simply a structure containing an opaque private pointer)
 * Trying to intermix these with varnish's Lck_* will quickly assert and abort.
 */
typedef struct vp_mutex vp_mutex_t;

/* call vp_mutex_init() on module load, can be called numerous times as long as each call
 * is paied with a vp_mutex_deinit().
 */
extern void vp_mutex_init(void);
extern void vp_mutex_deinit(void);
extern void vp_mutex_new_ex(struct lock*, struct VSC_C_lck*, const char*);
#define vp_mutex_new(l,s) vp_mutex_new_ex((l),lck_##s,"vmod_peer_"#s)
extern void vp_mutex_delete(struct lock*);
extern int vp_mutex_trylock_ex(struct lock*, const char *p, const char *f, int l);
#define vp_mutex_trylock(l) vp_mutex_trylock_ex((l), __func__, __FILE__, __LINE__)
extern void vp_mutex_lock_ex(struct lock*, const char *p, const char *f, int l);
#define vp_mutex_lock(l) vp_mutex_lock_ex((l), __func__, __FILE__, __LINE__)
extern void vp_mutex_unlock_ex(struct lock* ,const char *p, const char *f, int l);
#define vp_mutex_unlock(l) vp_mutex_unlock_ex((l), __func__, __FILE__, __LINE__)
extern void vp_mutex_assert(const struct lock*, int is_held);
#define vp_mutex_assertheld(l) vp_mutex_assert((l),1)
extern int vp_mutex_isheld(const struct lock*);
extern int vp_mutex_get_owner(const struct lock*, pthread_t*);
/* get_recursion_depth returns -1 if the mutex is not configured for same-thread recursion */
extern int vp_mutex_get_recursion_depth(const struct lock*);
/* set_recursion enables or disables same-thread recursion, returns 0 on succrss otherwise
 * return errno style error.
 */
extern int vp_mutex_set_recursion(struct lock*, int on, int *was_on);
extern void vp_mutex_condwait(pthread_cond_t*, struct lock*);


#endif /* VMOD_PEER_H */
