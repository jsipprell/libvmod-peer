#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "vmod_peer.h"

struct vp_lock {
  unsigned magic;
#define VP_LOCK_MAGIC 0x0ff198cb
#ifdef HAVE_PTHREAD_RWLOCKS
  pthread_rwlock_t l;
#else
  pthread_mutex_t m;
  pthread_cond_t c;
  enum {
    vp_lock_unset = 0,
    vp_lock_read,
    vp_lock_write
  } t;
  unsigned r,w;
  unsigned nw;
#endif
};

vp_lock_t *vp_lock_new(pthread_mutexattr_t *ma, pthread_condattr_t *ca)
{
  vp_lock_t *l;
#ifdef HAVE_PTHREAD_RWLOCKS
  pthread_rwlockattr_t attr;
#endif

  l = calloc(1,sizeof(*l));
  AN(l);
  l->magic = VP_LOCK_MAGIC;
#ifndef HAVE_PTHREAD_RWLOCKS
  l->r = l->w = l->nw = 0;
  l->t = vp_lock_unset;
  AZ(pthread_mutex_init(&l->m,ma));
  AZ(pthread_cond_init(&l->c,ca));
#else
  AZ(pthread_rwlockattr_init(&attr));
#ifdef HAVE_PTHREAD_RWLOCKATTR_SETPSHARED
  AZ(pthread_rwlockattr_setpshared(&attr,PTHREAD_PROCESS_PRIVATE));
#endif
  AZ(pthread_rwlock_init(&l->l,&attr));
  AZ(pthread_rwlockattr_destroy(&attr));
#endif
  return l;
}

void vp_lock_destroy(vp_lock_t *l)
{
  CHECK_OBJ_NOTNULL(l, VP_LOCK_MAGIC);

#ifndef HAVE_PTHREAD_RWLOCKS
  AZ(pthread_cond_destroy(&l->c));
  AZ(pthread_mutex_destroy(&l->m));
#else
  AZ(pthread_rwlock_destroy(&l->l));
#endif
  l->magic = 0;
  free(l);
}

int vp_lock_read_acquire(vp_lock_t *l)
{
  CHECK_OBJ_NOTNULL(l, VP_LOCK_MAGIC);

#ifndef HAVE_PTHREAD_RWLOCKS
  AZ(pthread_mutex_lock(&l->m));
  while(l->w > 0) {
    l->nw++;
    AZ(pthread_cond_wait(&l->c,&l->m));
    l->nw--;
  }
  AZ(l->w);
  l->r++;
  if(!l->t)
    l->t = vp_lock_read;
  return pthread_mutex_unlock(&l->m);
#else
  return pthread_rwlock_rdlock(&l->l);
#endif
}

int vp_lock_release(vp_lock_t *l)
{
  CHECK_OBJ_NOTNULL(l, VP_LOCK_MAGIC);

#ifndef HAVE_PTHREAD_RWLOCKS
  AZ(pthread_mutex_lock(&l->m));
  switch(l->t) {
  case vp_lock_read:
    assert(l->r > 0);
    l->r--;
    if(l->nw > 0 && l->w == 0 && l->r == 0)
      AZ(pthread_cond_broadcast(&l->c));
    break;
  case vp_lock_write:
    assert(l->w == 1);
    l->w--;
    if(l->nw > 0 && l->w + l->r > 0)
      AZ(pthread_cond_broadcast(&l->c));
    break;
  default:
    AZ("attempt to release unheld or invalid lock");
    pthread_mutex_unlock(&l->m);
    return -1;
  }
  l->t = vp_lock_unset;
  return pthread_mutex_unlock(&l->m);
#else
  return pthread_rwlock_unlock(&l->l);
#endif
}

int vp_lock_write_acquire(vp_lock_t *l)
{
  CHECK_OBJ_NOTNULL(l, VP_LOCK_MAGIC);

#ifndef HAVE_PTHREAD_RWLOCKS
  AZ(pthread_mutex_lock(&l->m));
  while(l->r + l->w > 0) {
    l->nw++;
    AZ(pthread_cond_wait(&l->c,&l->m));
    l->nw--;
  }
  AZ(l->r); AZ(l->w);
  l->w++;
  l->t = vp_lock_write;
  return pthread_mutex_unlock(&l->m);
#else
  return pthread_rwlock_wrlock(&l->l);
#endif
}
