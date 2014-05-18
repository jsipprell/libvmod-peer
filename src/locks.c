#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "vmod_peer.h"

struct vp_lock {
  unsigned magic;
#define VP_LOCK_MAGIC 0x0ff198cb
  pthread_mutex_t m;
  pthread_cond_t c;
  unsigned r,w;
  unsigned nw;
};

vp_lock_t *vp_lock_new(pthread_mutexattr_t *ma, pthread_condattr_t *ca)
{
  vp_lock_t *l;

  l = calloc(1,sizeof(*l));
  AN(l);
  l->magic = VP_LOCK_MAGIC;
  l->r = l->w = l->nw = 0;
  AZ(pthread_mutex_init(&l->m,ma));
  AZ(pthread_cond_init(&l->c,ca));
  return l;
}

void vp_lock_destroy(vp_lock_t *l)
{
  CHECK_OBJ_NOTNULL(l, VP_LOCK_MAGIC);
  AZ(pthread_cond_destroy(&l->c));
  AZ(pthread_mutex_destroy(&l->m));
  l->magic = 0;
  free(l);
}

int vp_lock_read_acquire(vp_lock_t *l)
{
  AZ(pthread_mutex_lock(&l->m));
  while(l->w > 0) {
    l->nw++;
    AZ(pthread_cond_wait(&l->c,&l->m));
    l->nw--;
  }
  AZ(l->w);
  l->r++;
  return pthread_mutex_unlock(&l->m);
}

int vp_lock_read_release(vp_lock_t *l)
{
  AZ(pthread_mutex_lock(&l->m));
  assert(l->r > 0);
  l->r--;
  if(l->nw > 0 && l->w == 0 && l->r == 0)
    AZ(pthread_cond_broadcast(&l->c));
  return pthread_mutex_unlock(&l->m);
}

int vp_lock_write_acquire(vp_lock_t *l)
{
  AZ(pthread_mutex_lock(&l->m));
  while(l->r + l->w > 0) {
    l->nw++;
    AZ(pthread_cond_wait(&l->c,&l->m));
    l->nw--;
  }
  AZ(l->r); AZ(l->w);
  l->w++;
  return pthread_mutex_unlock(&l->m);
}

int vp_lock_write_release(vp_lock_t *l)
{
  AZ(pthread_mutex_lock(&l->m));
  assert(l->w == 1);
  l->w--;
  if(l->nw > 0 && l->w + l->r > 0)
    AZ(pthread_cond_broadcast(&l->c));
  return pthread_mutex_unlock(&l->m);
}
