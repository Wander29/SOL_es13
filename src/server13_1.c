/* 13.1
 * Realizzare l'Esercizio 2 dell'Esercitazione 11 con un pool di N threads
 * (N è un parametro del programma) secondo il modello Manager-Workers.
 * Il generico thread Worker gestisce interamente tutta le richieste di un client connesso.
 * Gestire i segnali SIGINT e SIGQUIT per la terminazione consistente del server.
 *
 * 11.2
 * Realizzare un programma C che implementa un server che rimane sempre attivo in attesa
 * di richieste da parte di uno o piu' processi client su una socket di tipo AF_UNIX.
 * Ogni client richiede al server la trasformazione di tutti i caratteri di una stringa
 * da minuscoli a maiuscoli (es. ciao –> CIAO). Per ogni nuova connessione il server
 * lancia un thread POSIX che gestisce tutte le richieste del client
 * (modello “un thread per connessione”) e quindi termina la sua esecuzione quando
 * il client chiude la connessione.
 * Per testare il programma implementare uno script bash che lancia N>10 clients ognuno
 * dei quali invia una o piu' richieste al server multithreaded.
 */

#include <myutils.h>
#include <protocollo.h>
#include <mypthread.h>
#include <mysocket.h>
#include <concurrent_queue.h>

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>    

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>     
#include <ctype.h>

/*******************************************************************************
 *              LOGICA DELL'APPLICAZIONE                                       *
 *******************************************************************************
 * controllo parametro di ingresso
 * gestione segnali     -> thread handler
 * creo la thread pool  -> N thread
 * MANAGER: -crea il socket di ascolto e si mette in attesa
 *          -attende le richieste di connessione e invia su una coda FIFO
 *              condivisa con i worker (e quindi una coda concorrente) i fd relativi
 *              ai socket di comunicazione
 *          SE non vi sono più thread liberi nella thread pool
 *              -> spawn dinamico di thread
 * WORKER:  gestisce dall'inizio alla fine la comunicazione con 1 client
 *
 * TERMINAZIONE
 *      MANAGER: invia il messaggio di terminazione ai workers
 *               attende con una join gli N thread della thread pool
 *      cleanup generale
 *
 * SEGNALI:
 *     ricevendo un segnale SIGINT, SIGQUIT, SIGHUP o SIGTERM si dice al programma
 *     di terminare il programma in maniera safe. Vengono gestite le richieste correnti dei client,
 *      non ne vengono accettate di altre
 *******************************************************************************/

#define MAX_SIZE_TH_POOL    128
#define WORKER_EXIT_NUM     -29
#define MAX_BACKLOG         32

#define CONN_IO(v)                      \
    if((v) <= 0)                        \
        return ERRORE;

/******************************************************************************
 * VARIABILI GLOBALI, se accedute da più thread vengono gestite con delle lock
 */
 static int termina_server = 0;
 static int listen_ssfd;        /* listen server socket file descriptor
 *                                  globale per essere chiuso nel tsh,
 *                                  - avrei potuto incapsulare il relativo puntatore
 *                                  in una struttura per poi passarla
 *                                  al momento della creazione del thread - */
 static queue_t *Q;              /* coda FIFO di comunicazione fra Manager-Workers */
 static unsigned num_free_thread = 0;

/******************************************************************************
 * LOCK
 */
static pthread_mutex_t mtx_num_free_thread  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mtx_termina_server   = PTHREAD_MUTEX_INITIALIZER;

 /******************************************************************************
 * PROTOTIPI e FUNZIONI
 */
void *worker(void *arg);
int spawn_thread(int fdc);
void *manage_connection(void *arg);
conn_t do_request(buf_t *b, int fdc);
void strtoupper(char *s);

void cleanup() {
    MENO1(unlink(SOCKET_SERVER_NAME))
}

static inline int get_termina_server() {
    int r, err;
    PTH(err, pthread_mutex_lock(&mtx_termina_server))
    r = termina_server;
    PTH(err, pthread_mutex_unlock(&mtx_termina_server))

    return r;
}

static inline void set_termina_server_safe() {
    int err;
    PTH(err, pthread_mutex_lock(&mtx_termina_server))
    termina_server = 1;
    PTH(err, pthread_mutex_unlock(&mtx_termina_server))
}

static inline void ch_num_free_thread(const unsigned x) {
    int err;
    PTH(err, pthread_mutex_lock(&mtx_num_free_thread))
    num_free_thread += x;
    PTH(err, pthread_mutex_unlock(&mtx_num_free_thread))
}

static inline unsigned get_num_free_thread() {
    unsigned r;
    int err;
    PTH(err, pthread_mutex_lock(&mtx_num_free_thread))
    r = num_free_thread;
    PTH(err, pthread_mutex_unlock(&mtx_num_free_thread))

    return r;
}

/*
 * @INPUT   arg                 puntatore alla maschera di sengali mascherante i segnali
 *                              che questo thread signal handler gestirà
 * @EFFECTS aspetta in modo sincrono determinati segnali e li gestisce
 * @RETURNS NULL
 * !NOTES   dovrebbe essere chiamata solamente alla creazione di un singolo thread,
 *          al fine di gestire i segnali in modo sincrono
 */
void *sync_signal_handler(void *arg) {
    /*
     * - attende i segnali con sigwait
     * - se il segnale catturato è SIGINT, SIGQUIT, SIGTERM, SIGHUP
     *          setta la variabile che indica la terminazione del server
     *          chiude lo stream del socket di ascolto del server
     *          (in modo da interrompere eventuali call bloccanti come la accept() )
     */
    sigset_t *mask = (sigset_t *)arg;
    int sig_captured,           /* conterrà il segnale catturato dalla sigwait */
        err;                    /* usata per contenere valori di errore di ritorno */

    for(;;) {
        if(get_termina_server() == 1)
            break;

        PTH(err, sigwait(mask, &sig_captured))
        switch(sig_captured) {
            case SIGINT:
            case SIGQUIT:
            case SIGTERM:
            case SIGHUP:
                set_termina_server_safe();
                MENO1(shutdown(listen_ssfd, SHUT_RDWR))
                break;
            default: ;
        }
    }
    return (void *)NULL;
}

int main(int argc, char *argv[]) {
    assert(sizeof(sigset_t *) <= sizeof(void *));   /* ci accertiamo che non ci saranno errori di cast */
    /*******************************************************************
    * controllo parametro di ingresso N
    *******************************************************************/
    if(argc != 2) {
        printf("Usage: %s <N = numero thread nella thread pool, IN [1, %d]>\n", argv[0], MAX_SIZE_TH_POOL);
        exit(EXIT_FAILURE);
    }
    int size_th_pool = (int) strtol(argv[1], NULL, 10);
    if(size_th_pool < 1 || size_th_pool > MAX_SIZE_TH_POOL) {
        printf("Please insert a number for N IN [1, %d]>\n", MAX_SIZE_TH_POOL);
        exit(EXIT_FAILURE);
    }

    /*******************************************************************
    * gestione SEGNALI con un thread signal handler (TSH)
    *      - ignoro SIGPIPE
    *          - globale per il processo Server
    *      - maschero tutti i segnali che poi verranno gestiti dal TSH
    *          - la signal mask verrà ereditata da tutti i thread creati in futuro (è ciò che vogliamo)
    *      - creo il TSH
    ********************************************************************/

    /*
    * ignoro SIGPIPE
    */
    struct sigaction sa;
    MENO1(sigaction(SIGPIPE, NULL, &sa))
    sa.sa_handler = SIG_IGN;
    MENO1(sigaction(SIGPIPE, &sa, NULL))
    /*
    * maschero gli altri segnali che gestirà il TSH:
    *       SIGINT, SIGQUIT, SIGTERM, SIGHUP
    */
    sigset_t sig_mask;
    MENO1(sigemptyset(&sig_mask))
    MENO1(sigaddset(&sig_mask, SIGINT))
    MENO1(sigaddset(&sig_mask, SIGQUIT))
    MENO1(sigaddset(&sig_mask, SIGTERM))
    MENO1(sigaddset(&sig_mask, SIGHUP))

    int err;                    /* usata per contenere valori di errore di ritorno */
    PTH(err, pthread_sigmask(SIG_SETMASK, &sig_mask, NULL))
    /*
    * creo il tsh
    *      -eredita la signal_mask (è ciò che vogliamo)
    */
    pthread_t tid_tsh;          /* conterrà il TID del Thread Signal Handler */
    PTH(err, pthread_create(&tid_tsh, NULL, sync_signal_handler, (void *)&sig_mask))

    /********************************************************************
     * MANAGER
     * - crea il socket di ascolto e si mette in attesa
     * - attesa sulla accept() di connessioni da parte dei client
     * - quando un client si connette:
     *      - SE ci sono thread liberi:
     *              -> inserisce sulla coda FIFO il fd del socket di
     *                 comunicazione con quel client
     *      - SE non ci sono thread liberi
     *              -> spawna dinamicamente thread worker in
     *                 modalità detached
     *******************************************************************/
     /*
      * creazione e attivazione del Socket,
      *     installando un cleanup che all'uscita del server
      *     eliminerà il socket
      */
    SOCKETAF_UNIX(listen_ssfd)
    struct sockaddr_un listen_server_addr;
    SOCKADDR_UN(listen_server_addr, SOCKET_SERVER_NAME)

    atexit(cleanup);            /* registro un cleanup che cancellerà il socker all'uscita */

    MENO1(bind(listen_ssfd, (struct sockaddr *)&listen_server_addr, sizeof(listen_server_addr)))
    MENO1(listen(listen_ssfd, MAX_BACKLOG))

    MENO1(start_queue(&Q))

    /*****************************************************
    * creazione THREAD POOL
    *      - alloco le strutture dati necessarie per i thread worker
    *      - creo N thread
     *la comunicazione con il Manager avverrà per mezzo di una coda FIFO condivisa
     *
    *****************************************************/
    pthread_t *tid_ws;
    EQNULL(tid_ws = calloc(size_th_pool, sizeof(pthread_t)))
    int i;                      /* indice dei cicli for */
    for(i = 0; i < size_th_pool; i++) {
        PTH(err, pthread_create( (tid_ws + i), NULL, worker, (void *)NULL))
    }
    ch_num_free_thread(size_th_pool);

    /*
     * inizio dell'attività vera e propria del Manager
     */
    int com_csfd;               /* communication client socket file descriptor */
    for(;;) {
        if(get_termina_server() == 1)
            break;
        if( (com_csfd = accept(listen_ssfd, NULL, 0)) == -1 ) {
            if( (errno == EINVAL || errno == EINTR) && get_termina_server() == 1 ) {
                printf("[SERVER] chiusura server\n");
                break;
            } else {
                perror("accept");
                exit(EXIT_FAILURE);
            }
        }
        /*
         * richiesta di connessione accettata!
         *      - controllo numero thread attivi
         */
        if(get_num_free_thread() > 0) {
            /*
             * - inserisco la richiesta nella coda FIFO (mutua esclusione garantita
             *              dall'interfaccia)
             */
            insertFIFO(Q, (void *)com_csfd);
        } else {
            /*
             * - spawn dinamico di thread in modalità detached (così da non doverli attendere)
             */
            while(spawn_thread(com_csfd) == -1) {
                if(errno != EAGAIN) {
                    perror("spawn_thread");
                    exit(EXIT_FAILURE);
                }
            }
        }
    }

    /***********************************************************
    * TERMINAZIONE SERVER
    * - invio messaggio di terminazione ai workers
    * - attesa dei workers
    * - cleanup generale
    ***********************************************************/

    for(i=0; i < size_th_pool; i++)
        insertFIFO(Q, (void *) WORKER_EXIT_NUM);

    for(i=0; i < size_th_pool; i++) {
        PTH(err, pthread_join(tid_ws[i], NULL))
    }
    free(tid_ws);

    MENO1(close(listen_ssfd))
    free_queue(Q, NO_DYNAMIC_ELEMS);

    PTH(err, pthread_join(tid_tsh, NULL))

    PTH(err, pthread_mutex_destroy(&mtx_termina_server))
    PTH(err, pthread_mutex_destroy(&mtx_num_free_thread))
    
    return 0;
}

/*
 * @INPUT
 * @EFFECTS
 * @RETURNS
 */
void *worker(void *arg) {
    /********************************************************
     * - resta attivo per tutta la vita del server
     *      - prova a leggere dalla coda, se è vuota aspetta
     *      - SE il valore letto è il valore di terminazione
     *          -> effettua cleanup ed esce
     *      - ELSE aggiorna il numero di thread liberi
     *      - gestisce tutte le richieste di una connessione
     *      - alla terminazione della connessione chiude il socket
     *      - aggiorna il numero di thread liberi
     ********************************************************/

    for(;;) {
        if(get_termina_server() == 1)
            break;
        int fdc = (int) getFIFO(Q);                /* se la coda è vuota => chiamata bloccante */
        if(fdc == (int) NULL)
            continue;
        if(fdc == WORKER_EXIT_NUM)
            return (void *) NULL;

#ifdef SHOW_INFO
        fprintf(stderr, "\n[SERVER worker] connessione su fd [%d]\n", fdc);
#endif
        ch_num_free_thread(-1);
        manage_connection((void *)fdc);
        ch_num_free_thread(+1);

    }

    return (void *) NULL;
}

void *manage_connection(void *arg) {
    /*
     * gestione connessione, un singolo client
     * se arriva il segnale di terminazione:
     *      gestisce la richiesta corrente poi chiude il descrittore
     */
    int fdc = (int) arg;
    buf_t buffer;               /* buffer di comunicazione */
    buffer.len = BUFFER_SIZE;
    EQNULL(buffer.str = calloc(buffer.len, sizeof(char)))
    conn_t r;                   /* valore di ritorno dalla gestione della richiesta */

    do {
        if(get_termina_server() == 1)
            break;
        r = do_request(&buffer, fdc);
        if(r == ERRORE) {
            printf("[SERVER] errore durante comunicazione su fd [%d]\n", fdc);
            break;
        }
    } while(r != TERMINA);
    /*
    * terminazione connessione con il client
    */
    free(buffer.str);
    MENO1(close(fdc))

    return (void *) NULL;
}

/*
 * svolge una singola richiesta del client (comunicazione tramite socket AF_UNIX)
 * - legge la lunghezza del messaggio
 * - legge il messaggio vero e proprio
 * - SE il messaggio letto è di terminazione
 *      -> chiude la connessione
 * - elabora il messaggio
 * - scrive la lunghezza del messaggio
 * - scrive il messaggio elaborato
 */
conn_t do_request(buf_t *b, const int fdc) {
    unsigned len_read;
    /*
     * lettura dal socket
     * - lunghezza messaggio
     *      - controllo messaggio di terminazione
     * - invia risposta
     */
    CONN_IO(readn(fdc, &len_read, sizeof(unsigned)))
    CONN_IO(len_read)
    if(len_read > b->len) {
        b->len = len_read;
        EQNULL(b->str = realloc(b->str, b->len * sizeof(char)))
    }
    CONN_IO(readn(fdc, b->str, len_read * sizeof(char)))
    /*
     * controllo terminazione connessione
     */
    if(strcmp(b->str, QUIT_MESSAGE) == 0) {
        return TERMINA;
    }
    /*
     * - elaborazione messaggio da inviare
     * - invio messaggio elaborato
     */
    strtoupper(b->str);

    CONN_IO(writen(fdc, &len_read, sizeof(unsigned)))
    CONN_IO(writen(fdc, b->str, len_read * sizeof(char)))

    return CONTINUA;
}

void strtoupper(char *s) {
    char *j = s;
    while(*j != '\0') {
        *j =  isupper(*j) ? *j : toupper(*j);
        j++;
    }
}

int spawn_thread(int fdc) {
    /*
     * crea un thread che gestisce una singola connessione per poi temrinare
     *      -creato in modalità detached
     */
    pthread_attr_t th_attr;
    int err;
    if( (err = pthread_attr_init(&th_attr)) != 0) {
        MENO1(close(fdc))
        errno = EAGAIN;
        return -1;
    }
    if( (err = pthread_attr_setdetachstate(&th_attr, PTHREAD_CREATE_DETACHED)) != 0) {
        MENO1(close(fdc))
        errno = EAGAIN;
        PTH(err, pthread_attr_destroy(&th_attr))
        return -1;
    }
    pthread_t tid_tmp;
    if( (err = pthread_create(&tid_tmp, &th_attr, manage_connection, (void *)fdc )) != 0) {
        MENO1(close(fdc))
        errno = EFAULT;
        PTH(err, pthread_attr_destroy(&th_attr))
        return -1;
    }
#ifdef SHOW_INFO
    fprintf(stderr, "\n[SERVER] connessione su fd [%d] tramite Thread SPAWNED\n", fdc);
#endif

    return 0;
}