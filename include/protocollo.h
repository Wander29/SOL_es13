#ifndef INC_13_1_PROTOCOLLO_H
#define INC_13_1_PROTOCOLLO_H

#define SOCKET_SERVER_NAME  "./pleasekillmeatexit"
#define BUFFER_SIZE     256
#define QUIT_MESSAGE        "sevedemoAHGRANDE-29"

// #define DEBUG
#define SHOW_INFO
// #define SHOW_POLLFD
#define EXIT_INFO

typedef enum conn {
    ERRORE      = -1,
    CONTINUA    = 0,
    TERMINA     = 1
} conn_t;

typedef struct buf {
    unsigned len;
    char *str;
} buf_t;

#endif //INC_13_1_PROTOCOLLO_H
