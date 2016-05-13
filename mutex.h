#ifndef __PS_MUTEX_H__
#define __PS_MUTEX_H__

#include <pthread.h>
#include <errno.h>

extern int lock_debug;

/*! \brief PS mutex implementation */
typedef pthread_mutex_t ps_mutex;
/*! \brief PS mutex initialization */
#define ps_mutex_init(a) pthread_mutex_init(a,NULL)
/*! \brief PS mutex destruction */
#define ps_mutex_destroy(a) pthread_mutex_destroy(a)
/*! \brief PS mutex lock without debug */
#define ps_mutex_lock_nodebug(a) pthread_mutex_lock(a);
/*! \brief PS mutex lock with debug (prints the line that locked a mutex) */
#define ps_mutex_lock_debug(a) { printf("[%s:%s:%d:] ", __FILE__, __FUNCTION__, __LINE__); printf("LOCK %p\n", a); pthread_mutex_lock(a); };
/*! \brief PS mutex lock wrapper (selective locking debug) */
#define ps_mutex_lock(a) { if(!lock_debug) { ps_mutex_lock_nodebug(a); } else { ps_mutex_lock_debug(a); } };
/*! \brief PS mutex unlock without debug */
#define ps_mutex_unlock_nodebug(a) pthread_mutex_unlock(a);
/*! \brief PS mutex unlock with debug (prints the line that unlocked a mutex) */
#define ps_mutex_unlock_debug(a) { printf("[%s:%s:%d:] ", __FILE__, __FUNCTION__, __LINE__); printf("UNLOCK %p\n", a); pthread_mutex_unlock(a); };
/*! \brief PS mutex unlock wrapper (selective locking debug) */
#define ps_mutex_unlock(a) { if(!lock_debug) { ps_mutex_unlock_nodebug(a); } else { ps_mutex_unlock_debug(a); } };

/*! \brief PS condition implementation */
typedef pthread_cond_t ps_condition;
/*! \brief PS condition initialization */
#define ps_condition_init(a) pthread_cond_init(a,NULL)
/*! \brief PS condition destruction */
#define ps_condition_destroy(a) pthread_cond_destroy(a)
/*! \brief PS condition wait */
#define ps_condition_wait(a, b) pthread_cond_wait(a, b);
/*! \brief PS condition timed wait */
#define ps_condition_timedwait(a, b, c) pthread_cond_timedwait(a, b, c);
/*! \brief PS condition signal */
#define ps_condition_signal(a) pthread_cond_signal(a);
/*! \brief PS condition broadcast */
#define ps_condition_broadcast(a) pthread_cond_broadcast(a);

#endif /*__PS_MUTEX_H__*/
