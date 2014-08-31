#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vmod_peer.h"

#define SET_NULL_THREAD(t) t = (pthread_t)0

struct vp_mutex {
  unsigned magic;
#define VP_MUTEX_MAGIC 0x94bbefa6
  pthread_mutex_t m;
  int held;
  int recurse;
  pthread_t owner;
  VTAILQ_ENTRY(vp_mutex) list;
  const char *w;
  struct VSC_C_lck *stat;
};

static VTAILQ_HEAD(,vp_mutex) mtx_head = VTAILQ_HEAD_INITIALIZER(mtx_head);
static pthread_mutex_t mtx_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned load_cnt = 0;
static pthread_key_t tkey;

static void finalizer(pthread_t *tp, int rm)
{
  vp_mutex_t *m,*m2;
  AZ(pthread_mutex_lock(&mtx_mtx));
  pthread_t tid;

  if(tp)
    tid = *tp;
  else
    tid = pthread_self();

  VTAILQ_FOREACH_SAFE(m, &mtx_head, list, m2) {
    CHECK_OBJ_NOTNULL(m,VP_MUTEX_MAGIC);
    if(rm) {
      VTAILQ_REMOVE(&mtx_head,m,list);
      m->magic = 0;
      if(m->stat)
        m->stat->destroy++;
      if(m->held)
        pthread_mutex_unlock(&m->m);
      if (params->diag_bitmap & 0x8)
        VSL(SLT_Debug, 0, "VPM_DELETE(%s,%s,%d,%s) [FINAL]",__func__, __FILE__, __LINE__,
                                                                    m->w ? m->w : "N/A");
      m->recurse = -1;
      SET_NULL_THREAD(m->owner);
      m->stat = NULL;
      m->w = NULL;
      m->held = 0;
      AZ(pthread_mutex_destroy(&m->m));
      FREE_OBJ(m);
    } else if(m->held && pthread_equal(m->owner,tid)) {
      if (params->diag_bitmap & 0x8)
        VSL(SLT_Debug, 0, "VPM_DELETE(%s,%s,%d,%s) [final/thr:%p]",__func__,__FILE__,__LINE__,
                                                                  (m->w ? m->w : "N/A"),
                                                                  (void*)tid);
      pthread_mutex_unlock(&m->m);
      SET_NULL_THREAD(m->owner);
      if(m->recurse > 0)
        m->recurse = 0;
      m->w = NULL;
      m->held = 0;
    }
  }
  if(tp)
    free(tp);
  AZ(pthread_mutex_unlock(&mtx_mtx));
}

static void finalize_thread(void *tp)
{
  finalizer((pthread_t*)tp,0);
}

void vp_mutex_init(void)
{
  AZ(pthread_mutex_lock(&mtx_mtx));
  if(load_cnt++ == 0) {
    AZ(pthread_key_create(&tkey,finalize_thread));
  }
  AZ(pthread_mutex_unlock(&mtx_mtx));
}

void vp_mutex_deinit(void)
{
  pthread_t tid;
  AZ(pthread_mutex_lock(&mtx_mtx));
  if(--load_cnt == 0) {
    tid = pthread_self();
    pthread_setspecific(tkey,NULL);
    AZ(pthread_mutex_unlock(&mtx_mtx));
    finalizer(NULL,1);
    return;
  }
  AZ(pthread_mutex_unlock(&mtx_mtx));
}

void vp_mutex_new_ex(struct lock *lck, struct VSC_C_lck *st, const char *w)
{
  vp_mutex_t *m;

  AZ(lck->priv);
  ALLOC_OBJ(m, VP_MUTEX_MAGIC);
  AN(m);
  m->w = w;
  m->stat = st;
  m->recurse = -1;
  if(m->stat)
    m->stat->creat++;
  AZ(pthread_mutex_init(&m->m, NULL));
  if (params->diag_bitmap & 0x8)
    VSL(SLT_Debug, 0, "VPM_NEW(%s,%s,%d,%s)",__func__, __FILE__, __LINE__, w ? w : "N/A");

  AZ(pthread_mutex_lock(&mtx_mtx));
  VTAILQ_INSERT_TAIL(&mtx_head,m,list);
  AZ(pthread_mutex_unlock(&mtx_mtx));
  lck->priv = m;
}

void vp_mutex_delete(struct lock *lck)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m, lck->priv, VP_MUTEX_MAGIC);
  if (params->diag_bitmap & 0x8)
    VSL(SLT_Debug, 0, "VPM_DELETE(%s,%s,%d,%s)",__func__, __FILE__, __LINE__, m->w ? m->w : "N/A");
  if(m->stat)
    m->stat->destroy++;
  AZ(pthread_mutex_lock(&mtx_mtx));
  VTAILQ_REMOVE(&mtx_head, m, list);
  AZ(pthread_mutex_unlock(&mtx_mtx));
  if(m->held && pthread_equal(m->owner,pthread_self())) {
    if(m->recurse < 0)
      AZ(pthread_mutex_unlock(&m->m));
    else {
      for(; m->recurse > 0; m->recurse--)
        AZ(pthread_mutex_unlock(&m->m));
    }
    m->held = 0;
    m->recurse = -1;
    SET_NULL_THREAD(m->owner);
    m->stat = NULL;
    m->w = NULL;
  }
  AZ(pthread_mutex_destroy(&m->m));
  FREE_OBJ(m);
}

int vp_mutex_trylock_ex(struct lock *lck, const char *p, const char *f, int l)
{
  pthread_t *tp,tid = pthread_self();
  vp_mutex_t*m;
  int r = 0;
  int e = 0;

  CAST_OBJ_NOTNULL(m, lck->priv, VP_MUTEX_MAGIC);
  if(m->held && pthread_equal(tid,m->owner)) {
    if(m->recurse == -1) {
      VSL(SLT_Error, 0, "VPM_TRYLOCK(%s,%s,%d,%s) = attempt to recursively lock non-recursive mutex",
          p,f,l,m->w);
      e = EINVAL;
      goto vp_mutex_trylock_fini;
    }
    assert(m->recurse > 1);
    m->recurse++;
    if(params->diag_bitmap & 0x8)
      VSL(SLT_Debug, 0, "VPM_TRYLOCK(%s,%s,%d,%s)/%u",
                        p,f,l,m->w,(unsigned)m->recurse);
    e = 0;
    goto vp_mutex_trylock_fini;
  }
  r = pthread_mutex_trylock(&m->m);
  if(r == 0) {
    AZ(m->held);
    m->held = 1;
    if(m->recurse > -1) {
      if(params->diag_bitmap & 0x8)
        VSL(SLT_Debug, 0, "VPM_TRYLOCK(%s,%s,%d,%s)/%u",
                          p,f,l,m->w,(unsigned)m->recurse+1);
      m->recurse++;
      assert(pthread_equal(tid,m->owner));
      if(m->recurse > 1) {
        /* don't much with TLS after the first level of recursion as it's never necessary */
        goto vp_mutex_trylock_fini;
      }
    } else if(params->diag_bitmap & 0x8) {
      VSL(SLT_Debug, 0, "VPM_TRYLOCK(%s,%s,%d,%s)",
                          p,f,l,m->w);

    }
    tp = pthread_getspecific(tkey);
    m->owner = tid;
    if(!tp) {
      tp = malloc(sizeof(*tp));
      *tp = tid;
      AZ(pthread_setspecific(tkey,tp));
    } else assert(pthread_equal(tid,*tp));
  }
vp_mutex_trylock_fini:
  if(!r && e)
    r = e;
  return r;
}

void vp_mutex_lock_ex(struct lock *lck, const char *p,const char *f, int l)
{
  pthread_t tid,*tp;
  vp_mutex_t *m;
  int r;

  CAST_OBJ_NOTNULL(m, lck->priv, VP_MUTEX_MAGIC);
  if(m->held && pthread_equal(m->owner,pthread_self()) && m->recurse > -1) {
    m->recurse++;
    if(params->diag_bitmap & 0x18)
      VSL(SLT_Debug, 0, "VPM_LOCK(%s,%s,%d,%s)/%u",
                        p,f,l,m->w,(unsigned)m->recurse);
    return;
  }
  if (!(params->diag_bitmap & 0x18)) {
    AZ(pthread_mutex_lock(&m->m));
    AZ(m->held);
    if(m->stat)
      m->stat->locks++;
    m->owner = pthread_self();
    m->held = 1;
    if(m->recurse > -1)
      m->recurse++;
    return;
  }

  tid = pthread_self();
  r = pthread_mutex_trylock(&m->m);
  assert(r == 0 || r == EBUSY);
  if(r) {
    if(m->stat)
      m->stat->colls++;
    if(params->diag_bitmap & 0x8) {
      if(m->recurse == -1)
        VSL(SLT_Debug, 0, "VPM_COLLISON(%s,%s,%d,%s)",
            p,f,l,m->w);
      else
        VSL(SLT_Debug, 0, "VPM_COLLISON(%s,%s,%d,%s)/%u",
            p,f,l,m->w,(unsigned)m->recurse);
    }
    AZ(pthread_mutex_lock(&m->m));
  } else if (params->diag_bitmap & 0x8)
    VSL(SLT_Debug, 0, "VPM_LOCK(%s,%s,%d,%s)",p,f,l,m->w);
  if(m->recurse > 0) {
    m->recurse++;
    AN(m->held);
    assert(pthread_equal(m->owner,tid));
  } else {
    if(m->stat)
      m->stat->locks++;
    m->owner = tid;
    m->held = 1;
    if(m->recurse == 0)
      m->recurse++;
  }
  if(!pthread_getspecific(tkey)) {
    tp = malloc(sizeof(*tp));
    AN(tp);
    *tp = tid;
    AZ(pthread_setspecific(tkey,tp));
  }
}

void vp_mutex_unlock_ex(struct lock *lck, const char *p, const char *f, int l)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m ,lck->priv, VP_MUTEX_MAGIC);
  AN(m->held);
  assert(pthread_equal(m->owner,pthread_self()));
  if ((params->diag_bitmap & 0x8) && m->recurse < 1)
    VSL(SLT_Debug, 0, "VPM_UNLOCK(%s,%s,%d,%s)",p,f,l,m->w);
  if(m->recurse > 0) {
    if(params->diag_bitmap & 0x8)
      VSL(SLT_Debug, 0, "VPM_UNLOCK(%s,%s,%d,%s)/%u",p,f,l,m->w,
          (unsigned)m->recurse-1);
    if(--(m->recurse) > 0)
      return;
  }
  m->held = 0;
  SET_NULL_THREAD(m->owner);
  AZ(pthread_mutex_unlock(&m->m));
}

void vp_mutex_assert(const struct lock *lck, int held)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m ,lck->priv, VP_MUTEX_MAGIC);
  if(m->held) {
    assert(m->held && pthread_equal(m->owner,pthread_self()));
  }  else {
    assert(!m->held || !pthread_equal(m->owner, pthread_self()));
  }
}

int vp_mutex_isheld(const struct lock *lck)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m, lck->priv, VP_MUTEX_MAGIC);
  return  (m->held && pthread_equal(m->owner,pthread_self()));
}

int vp_mutex_get_owner(const struct lock *lck, pthread_t *tp)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m ,lck->priv, VP_MUTEX_MAGIC);
  if(!m->held)
    return ENOENT;
  AN(tp);
  *tp = m->owner;
  return 0;
}

int vp_mutex_get_recursion_depth(const struct lock *lck)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m ,lck->priv, VP_MUTEX_MAGIC);
  return m->recurse;
}

int vp_mutex_set_recursion(struct lock *lck, int on, int *state)
{
  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m ,lck->priv, VP_MUTEX_MAGIC);
  if(m->held && !pthread_equal(m->owner,pthread_self()))
    return EBUSY;
  else {
    if(state)
        *state = (m->recurse == -1 ? 0 : 1);
    if(on && m->recurse == -1)
      m->recurse = 0;
    else if(!on && m->recurse == 0)
      m->recurse = -1;
    else
      return EINVAL;
  }
  return 0;
}

void vp_mutex_condwait(pthread_cond_t *cond, struct lock *lck)
{
  pthread_t tid = pthread_self();
  pthread_t *tp;

  vp_mutex_t *m;
  CAST_OBJ_NOTNULL(m, lck->priv, VP_MUTEX_MAGIC);
  AN(m->held);
  assert(pthread_equal(m->owner, tid));
  m->held = 0;
  if(params->diag_bitmap & 0x8)
    VSL(SLT_Debug, 0, "VPM_WAIT(%s,%d,%s)/%d",__FILE__,__LINE__,m->w,
          (int)m->recurse);
  AZ(pthread_cond_wait(cond, &m->m));
  AZ(m->held);
  tp = pthread_getspecific(tkey);
  if(!tp) {
    tp = malloc(sizeof(*tp));
    AN(tp);
    *tp = tid;
    AZ(pthread_setspecific(tkey,tp));
  }
  m->owner = tid;
  m->held = 1;
  if(params->diag_bitmap & 0x8)
    VSL(SLT_Debug, 0, "VPM_WAKE(%s,%d,%s)/%d",__FILE__,__LINE__,m->w,
          (int)m->recurse);
}
