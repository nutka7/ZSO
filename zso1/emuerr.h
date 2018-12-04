#ifndef EMUERR_H
#define EMUERR_H

#include <string.h>
#include <stdio.h>
#include <errno.h>

#define EEMU 127
#define LOG_FILENAME "emulog"


#define LOG(Stream, Format, ...) \
	fprintf(Stream, "%s:%u: " Format "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define LOGTOFILE(...)             \
do {                               \
	FILE *_f;                      \
                                   \
	_f = fopen(LOG_FILENAME, "a"); \
	if (_f) LOG(_f, __VA_ARGS__);  \
	if (_f) fclose(_f);            \
} while (0)

#define EMUERR(...)             \
do {                            \
        LOGTOFILE(__VA_ARGS__); \
        exit(EEMU);             \
} while (0)

#define SYSERR(Rval, Format, ...)                                  \
do {                                                               \
    if ((Rval) == -1) {                                            \
        LOGTOFILE(Format " : %s", ##__VA_ARGS__, strerror(errno)); \
        exit(EEMU);                                                \
    }                                                              \
} while (0)

#define SYS2ERR(Rval, Format, ...)                                 \
do {                                                               \
    if ((Rval) == (void *) -1) {                                   \
        LOGTOFILE(Format " : %s", ##__VA_ARGS__, strerror(errno)); \
        exit(EEMU);                                                \
    }                                                              \
} while (0)

#define NERR(Rval)            \
do {                          \
	if ((Rval) == ERR) {      \
		LOGTOFILE("ncurses"); \
		exit(EEMU);           \
	}                         \
} while (0)

#endif // EMUERR_H
