#ifndef __LOG_H__
#define __LOG_H__
void printf(char *, ...);

// #define LOG_LEVEL_NONE
#define LOG_LEVEL_CRITICAL
// #define LOG_LEVEL_DEBUG
// #define LOG_LEVEL_INFO
// #define LOG_LEVEL_TRACE
// #define LOG_LEVEL_ALL

#if defined(LOG_LEVEL_CRITICAL)

#define USE_LOG_ERROR
#define USE_LOG_WARN

#endif // LOG_LEVEL_CRITICAL

#if defined(LOG_LEVEL_DEBUG)

#define USE_LOG_ERROR
#define USE_LOG_WARN
#define USE_LOG_DEBUG

#endif // LOG_LEVEL_DEBUG

#if defined(LOG_LEVEL_INFO)

#define USE_LOG_INFO

#endif // LOG_LEVEL_INFO

#if defined(LOG_LEVEL_TRACE)

#define USE_LOG_INFO
#define USE_LOG_TRACE

#endif // LOG_LEVEL_TRACE

#if defined(LOG_LEVEL_ALL)

#define USE_LOG_WARN
#define USE_LOG_ERROR
#define USE_LOG_INFO
#define USE_LOG_DEBUG
#define USE_LOG_TRACE

#endif // LOG_LEVEL_ALL

enum LOG_COLOR
{
    RED = 31,
    GREEN = 32,
    BLUE = 34,
    GRAY = 90,
    YELLOW = 93,
};

#if defined(USE_LOG_WARN)

#define warnf(fmt, ...) printf("\x1b[%dm[%s] " fmt "\x1b[0m\n", YELLOW, "WARN", ##__VA_ARGS__);
#else
#define warnf(fmt, ...)
#endif //

#if defined(USE_LOG_ERROR)

#define errorf(fmt, ...) printf("\x1b[%dm[%s] " fmt "\x1b[0m\n", RED, "ERROR", ##__VA_ARGS__);
#else
#define errorf(fmt, ...)
#endif //

#if defined(USE_LOG_DEBUG)

#define debugf(fmt, ...) printf("\x1b[%dm[%s] " fmt "\x1b[0m\n", GREEN, "DEBUG", ##__VA_ARGS__);

#define debugcore(fmt, ...) printf("\x1b[%dm[%s %d] " fmt "\x1b[0m\n", GREEN, "DEBUG", cpuid(), ##__VA_ARGS__);
#define phex(var_name) debugf(#var_name "=%p", var_name)

#else
#define debugf(fmt, ...)
#define debugcore(fmt, ...)
#endif //

#if defined(USE_LOG_TRACE)

#define tracef(fmt, ...) printf("\x1b[%dm[%s] " fmt "\x1b[0m\n", GRAY, "TRACE", ##__VA_ARGS__);
#else
#define tracef(fmt, ...)
#endif //

#if defined(USE_LOG_INFO)

#define infof(fmt, ...) printf("\x1b[%dm[%s] " fmt "\x1b[0m\n", BLUE, "INFO", ##__VA_ARGS__);
#else
#define infof(fmt, ...)
#endif //

#endif //!__LOG_H__