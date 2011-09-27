/* $Id: stopwatch.h 27450 2010-08-05 19:02:48Z twu $ */
#ifndef STOPWATCH_INCLUDED
#define STOPWATCH_INCLUDED

#define T Stopwatch_T
typedef struct T *T;

extern T
Stopwatch_new ();
extern void
Stopwatch_free (T *old);
extern void
Stopwatch_start (T this);
extern double 
Stopwatch_stop (T this);

#undef T
#endif


