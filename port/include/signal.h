#ifndef PORT_SIGNAL_H
#define PORT_SIGNAL_H

/* There are no signals on a console; signal() accepts and ignores. */
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGTERM 15

sighandler_t signal(int sig, sighandler_t handler);

#endif
