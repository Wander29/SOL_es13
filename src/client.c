#include <myutils.h>
#include <protocollo.h>
#include <mysocket.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // read / write

/*********************************************************
 * - ricrea il socket id comunicazione col server
 * - si connette al server
 * - invia una richiesta al server PER OGNI stringa
 *      passata al programma
 * - termina la connessione inviando un messaggio
 *      di terminazione definito nel protocollo
 ********************************************************/

void send_quit_message(buf_t *b, int fdc);

int main(int argc, char *argv[]) {
    assert(sizeof(int) <= sizeof(void *));
    /*
     * controllo valori in input al programma
     */
    if(argc < 2) {
        printf("USAGE: %s <stringa1> [<stringa2> ... <stringaM>]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    /*
     * connessione al server tramite socket e indirizzo
     */
    int fdc;
    SOCKETAF_UNIX(fdc)
    struct sockaddr_un serv_addr;
    SOCKADDR_UN(serv_addr, SOCKET_SERVER_NAME)

    while(connect(fdc, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
        if(errno == ENOENT) { // se il server non è ancora stato creato
            sleep(1);
        } else {
            perror("connect");
            exit(EXIT_FAILURE);
        }
    }
#ifdef SHOW_INFO
    fprintf(stderr, "\n[CLIENT] connessione accettata\n");
#endif
    /**********************************************
     * invio delle richieste al server
     * - invio la lunghezza del messaggio
     * - invio il messaggio vero e proprio
     * attesa della risposta
     * - leggo la lunghezza della risposta
     * - leggo la risposta
     *********************************************/
    buf_t buffer;
    buffer.len = BUFFER_SIZE;
    EQNULL(buffer.str = calloc(buffer.len, sizeof(char)))
    unsigned len_current;
    for(int i=1; i < argc; i++) {
        len_current = strlen(argv[i]) + 1;
        MENO1(writen(fdc, &len_current, sizeof(unsigned)))
#ifdef DEBUG
        fprintf(stderr, "\n[CLIENT %d] scrive : %d\n", getpid(), len_current);
#endif
        MENO1(writen(fdc, argv[i], len_current * sizeof(char)))
#ifdef DEBUG
        fprintf(stderr, "\n[CLIENT %d] scrive: %s\n", getpid(), argv[i]);
#endif

        MENO1(readn(fdc, &len_current, sizeof(unsigned)))
#ifdef DEBUG
        fprintf(stderr, "\n[CLIENT %d] legge: %d\n", getpid(), len_current);
#endif
        if(len_current > buffer.len) {
            buffer.len = len_current;
            EQNULL(buffer.str = realloc(buffer.str, buffer.len * sizeof(char)))
        }
        MENO1(readn(fdc, buffer.str, len_current * sizeof(char)))
#ifdef DEBUG
        fprintf(stderr, "\n[CLIENT %d] legge: %s\n", getpid(), buffer.str);
#endif
        printf("[CLIENT %d] %s\n", getpid(), buffer.str);
    }
    /*
     * !) terminazione
     * - invio messaggio di chiusura connessione
     * - cleanup
     */
    send_quit_message(&buffer, fdc);

    free(buffer.str);
    MENO1(close(fdc))

    return 0;
}

void send_quit_message(buf_t *b, int fdc) {
    int len_quit_msg = strlen(QUIT_MESSAGE) + 1;
    free(b->str);
    EQNULL(b->str = strndup(QUIT_MESSAGE, len_quit_msg))
    b->len = len_quit_msg;

    MENO1(writen(fdc, &len_quit_msg, sizeof(unsigned)))
    MENO1(writen(fdc, b->str, len_quit_msg * sizeof(char)))
}
