#define ENABLED (1)
#ifdef ENABLED
typedef int EnabledOn;
#endif
#ifdef MISSING
typedef int MissingOn;
#endif
EnabledOn * p;
