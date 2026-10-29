/* sipwire: replaces SpeexDSP's libc allocation and stdio diagnostics
 * (included by os_support.h when OS_SUPPORT_CUSTOM is defined, see config.h).
 * The hooks are implemented in components/aec/aec.c. */
#ifndef SIPWIRE_SPEEXDSP_OS_SUPPORT_CUSTOM_H
#define SIPWIRE_SPEEXDSP_OS_SUPPORT_CUSTOM_H

void *aec_speex_alloc(int size);            /* zeroed, through aec_set_allocator() */
void aec_speex_free(void *ptr);
void aec_speex_warning(const char *str);    /* counted, not printed */
void aec_speex_fatal(const char *str);      /* aborts */

#define OVERRIDE_SPEEX_ALLOC
static inline void *speex_alloc(int size)
{
    return aec_speex_alloc(size);
}

#define OVERRIDE_SPEEX_ALLOC_SCRATCH
static inline void *speex_alloc_scratch(int size)
{
    return aec_speex_alloc(size);
}

/* speex_realloc: not used by the vendored files, left to os_support.h */

#define OVERRIDE_SPEEX_FREE
static inline void speex_free(void *ptr)
{
    aec_speex_free(ptr);
}

#define OVERRIDE_SPEEX_FREE_SCRATCH
static inline void speex_free_scratch(void *ptr)
{
    aec_speex_free(ptr);
}

#define OVERRIDE_SPEEX_FATAL
static inline void _speex_fatal(const char *str, const char *file, int line)
{
    (void)file;
    (void)line;
    aec_speex_fatal(str);
}

#define OVERRIDE_SPEEX_WARNING
static inline void speex_warning(const char *str)
{
    aec_speex_warning(str);
}

#define OVERRIDE_SPEEX_WARNING_INT
static inline void speex_warning_int(const char *str, int val)
{
    (void)val;
    aec_speex_warning(str);
}

#define OVERRIDE_SPEEX_NOTIFY
static inline void speex_notify(const char *str)
{
    (void)str;
}

#define OVERRIDE_SPEEX_PUTC
static inline void _speex_putc(int ch, void *file)
{
    (void)ch;
    (void)file;
}

#endif
