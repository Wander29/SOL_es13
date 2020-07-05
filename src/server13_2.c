/* 13.1
 * Realizzare l'Esercizio 2 dell'Esercitazione 11 con un pool di N threads
 * (N è un parametro del programma) secondo il modello Manager-Workers.
 * Il generico thread Worker gestisce interamente tutta le richieste di un client connesso.
 * Gestire i segnali SIGINT e SIGQUIT per la terminazione consistente del server.
 *
 * 13.2
 * Realizzare una seconda versione dell'Esercizio 1 (sempre secondo lo schema
 * Manager-Workers con thread pool) in cui il generico thread Worker gestisce
 * solamente una richiesta di uno dei client connessi (non c'è una associazione
 * fissa tra thread Worker e client).
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
#include <mypoll.h>
#include <mysocket.h>
#include <concurrent_queue.h>

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>     // read / write

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>     //

/*******************************************************************************
 *              LOGICA DELL'APPLICAZIONE                                       *
 *******************************************************************************
 * controllo parametro di ingresso
 * gestione segnali     -> thread handler
 * creo la thread pool  -> N thread
 * MANAGER: -crea il socket di ascolto e si mette in attesa
 *          -attende le richieste di connessione, le accetta e si mette in
 *              attesa di richieste di lavoro
 *          - attende richieste di laovoro  e invia su una coda FIFO
 *              condivisa con i worker (e quindi una coda concorrente) i fd relativi
 *              ai socket di comunicazione
 *          SE non vi sono più thread liberi nella thread pool
 *              -> spawn dinamico di thread
 * WORKER:  gestisce una singola richiesta di un client
*           - una volta eseguita ritorna il fd, specfificando se la
 *              connessione è terminata o ci si aspettano altre richieste su quel fd
 *
 * TERMINAZIONE
 *      MANAGER: invia il messaggio di terminazione ai workers
 *               attende con una join gli N thread della thread pool
 *      cleanup generale
 *
 * SEGNALI:
 *     ricevendo un segnale SIGINT, SIGQUIT, SIGHUP o SIGTERM si dice al programma
 *     di terminare il programma in maniera safe. Vengono gestite le richieste correnti dei client,
 *      non ne vengono accettate altre
 *******************************************************************************/

#define MAX_SIZE_TH_POOL    128
#define WORKER_EXIT_NUM     -29
#define MAX_BACKLOG         32

#define CONN_IO(v)                      \
    if((v) <= 0) {                      \
        pthread_mutex_lock(&debug_mtx); \
        fprintf(stderr, "\n[WORKER %d] ERRORE lettura su fd [%d]\n", syscall(__NR_gettid), fdc);   \
        perror(#v);                     \
        pthread_mutex_unlock(&debug_mtx);   \
        return ERRORE;                  \
    }


/*
 * tipo di elementi utilizzati nella pipe senza nome
 */
typedef struct elem {
    int fd;
    conn_t conn_state;
} elem_pipe_t;

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
static int pipefd[2];              /* pipefd[0]: lettura, pipefd[1]: scrittura */

static int max_open_files = 0;      /* per debug */

/******************************************************************************
 * LOCK
 */
static pthread_mutex_t mtx_num_free_thread  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mtx_termina_server   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mtx_pipe             = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t debug_mtx            = PTHREAD_MUTEX_INITIALIZER;

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
#ifdef EXIT_INFO
    printf("## MAX num_open_files [%d]\n", max_open_files);
#endif
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

/**
 * @INPUT   //
 * @EFFECTS legge dalla pipe in mutua esclusione un elemento elem_pipe_t
 * @return  l'elemento avente i campi letti
 */
int get_pipe(elem_pipe_t *tmp) {
    int err;
    PTH(err, pthread_mutex_lock(&mtx_pipe))

    if( readn(pipefd[0], &(tmp->fd), sizeof(int)) == 0) {
        return -1;
    }
    if( readn(pipefd[0], &(tmp->conn_state), sizeof(conn_t)) == 0) {
        return -1;
    }

    PTH(err, pthread_mutex_unlock(&mtx_pipe))

    return 0;
}

int insert_pipe(elem_pipe_t tmp) {
    int err;
    PTH(err, pthread_mutex_lock(&mtx_pipe))

    if( writen(pipefd[1], &(tmp.fd), sizeof(int)) == 0 )
        return -1;
    if( writen(pipefd[1], &(tmp.conn_state), sizeof(conn_t)) == 0 )
        return -1;
#ifdef DEBUG
    fprintf(stderr, "\n[PIPE] scrittura: fd [%d] con valore [%d]\n", tmp.fd, tmp.conn_state);
#endif
    PTH(err, pthread_mutex_unlock(&mtx_pipe))

    return 0;
}

/**
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
    assert(sizeof(sigset_t *)   <= sizeof(void *));
    assert(sizeof(int)          <= sizeof(void *));   /* ci accertiamo che non ci saranno errori di cast */
    /*******************************************************************
    * controllo parametro di ingresso N
    *******************************************************************/
    if(argc != 2) {
        printf("Usage: %s <N = numero thread nella thread pool, IN [1, %d]>\n", argv[0], MAX_SIZE_TH_POOL);
        exit(EXIT_FAILURE);
    }
    int size_th_pool = (int) strtol(argv[1], NULL, 10);
    if(size_th_pool < 0 || size_th_pool > MAX_SIZE_TH_POOL) {
        printf("Please insert a number for N IN [1, %d]>\n", MAX_SIZE_TH_POOL);
        exit(EXIT_FAILURE);
    }

    /*******************************************************************
    * gestione SEGNALI con un thread signal handler (TSH)
    *      - ignoro SIGPIPE
    *          - globale per il processo Server
    *      - maschero tutti i segnali che poi verranno gestiti dal TSH
    *          - la signal mask verrà ereditata da tutti i thread creati in
     *              futuro (è ciò che vogliamo)
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
     * - MULTIPLEXING: attende tramite POLL I/O su:
     *          . listen socket
     *          . socket di comunicazione
     *          . pipe di comunicazione con i workers
     *
     * - con accept() accetta connessioni da parte dei client
     * - quando un client fa una richiesta di lavoro:
     *      - SE ci sono thread liberi:
     *              -> inserisce sulla coda FIFO il fd del socket di
     *                 comunicazione con quel client
     *      - SE non ci sono thread liberi
     *              -> spawna dinamicamente thread worker in
     *                 modalità detached passandogli il relativo fd
     *                 così da poter eseguire la singola richiesta
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

    /*
     * inizializzazione canali di comunicazione fra Manager e Workers
     *      - attivo la coda FIFO concorrente
     *      - creo la pipe senza nome
     *      - in quanto solo lettore il manager dovrebbe chiudere pipefd[1] MA
     *              fra thread, essendo la tabella dei fd unica per processo,
     *              lo chiuderei per tutti
     */
    MENO1(start_queue(&Q))
    MENO1(pipe(pipefd))
    /*
     * setto il flag O_NONBLOCK sulla pipe in modo da non avere I/O bloccanti
     * - pipefd[0]
     * - pipefd[1]
     */
    int flags;
    MENO1(flags = fcntl(pipefd[0], F_GETFL))        /* prende i vecchi flag */
    flags |= O_NONBLOCK;                            /* abilita il bit O_NONBLOCK */
    MENO1(fcntl(pipefd[0], F_SETFL, flags))         /* aggiorna i flag */

    MENO1(flags = fcntl(pipefd[1], F_GETFL))        /* prende i vecchi flag */
    flags |= O_NONBLOCK;                            /* abilita il bit O_NONBLOCK */
    MENO1(fcntl(pipefd[1], F_SETFL, flags))         /* aggiorna i flag */

    /*****************************************************
    * creazione THREAD POOL
    *      - alloco le strutture dati necessarie per i thread worker
    *      - creo N thread
     *la comunicazione con il Manager avverrà per mezzo
     *      di una coda FIFO condivisa e con una pipe senza nome
    *****************************************************/
    pthread_t *tid_ws;
    EQNULL(tid_ws = calloc(size_th_pool, sizeof(pthread_t)))
    int i;                      /* indice dei cicli for */
    for(i = 0; i < size_th_pool; i++) {
        PTH(err, pthread_create( (tid_ws + i), NULL, worker, (void *)NULL))
    }
    ch_num_free_thread(size_th_pool);

    /**************************************************************
     * inizio dell'attività vera e propria del Manager
     *      - inizializzazione strutture per la POLL
     *      - con la POLL inizio ascoltando
     *          listen socket
     *          pipe[0]
     **************************************************************/

    struct pollfd *pollfd_v;    /* array di pollfd */
    EQNULL(pollfd_v = start_pollfd())
    int polled_fd = 0;          /* conterrà il numero di fd che si stanno monitorando */
    struct pollfd tmp;          /* pollfd di supporto per gli inserimenti */

    tmp.fd = listen_ssfd;
    tmp.events = POLLIN;        /* attende eventi di lettura non con alta priorità, sul listen socket */
    MENO1(pollfd_add(&pollfd_v, tmp, &polled_fd))

    tmp.fd = pipefd[0];
    tmp.events = POLLIN;
    MENO1(pollfd_add(&pollfd_v, tmp, &polled_fd))

    int cnt_open_files = 0;

    int com_csfd;               /* communication client socket file descriptor */
    for(;;) {
        if(get_termina_server() == 1)
            break;
        /*
         * MULTIPLEXING: attendo I/O su vari fd
         */
        if(poll(pollfd_v, polled_fd, -1) == -1) {       /* aspetta senza timeout, si blocca */
            if(errno == EINTR && get_termina_server() == 1) {
                printf("[SERVER] chiusura server\n");
                break;
            } else {
                perror("accept");
                exit(EXIT_FAILURE);
            }
        }
        /*
         * ci sono fd su cui è possibile effettura un'operazione I/O, li controllo
         */
        int current_pollfd_array_size = polled_fd;  /* il cnt di fd monitorati può cambiare durante il ciclo */
        for(i=0; i < current_pollfd_array_size; i++) {
            if(pollfd_v[i].revents & POLLIN) {      /* fd pronto per la lettura! */
                int fd_curr = pollfd_v[i].fd;

                if(fd_curr == listen_ssfd) { /* accetto la connessione e monitoro il nuovo socket */
                    if( (com_csfd = accept(listen_ssfd, NULL, 0)) == -1 ) {
                        if( (errno == EINVAL || errno == EINTR) && get_termina_server() == 1 ) {
                            printf("[SERVER] chiusura server\n");
                            break;
                        } else {
                            perror("Accept");
                            exit(EXIT_FAILURE);
                        }
                    }
                    tmp.fd = com_csfd;
                    tmp.events = POLLIN;
                    MENO1(pollfd_add(&pollfd_v, tmp, &polled_fd))
#ifdef SHOW_INFO
                    pthread_mutex_lock(&debug_mtx);
                    fprintf(stderr, "\n[MANAGER] accept su fd [%d]\n", com_csfd);
                    print_pollfd(pollfd_v, polled_fd);
                    pthread_mutex_unlock(&debug_mtx);
#endif
                    cnt_open_files++;
                    if(cnt_open_files > max_open_files)
                        max_open_files = cnt_open_files;
                } else if(fd_curr == pipefd[0]) {    /* leggo dalla pipe comunicazione di un worker */
                    elem_pipe_t ans;
                    MENO1(get_pipe(&ans))
#ifdef SHOW_INFO_MANAGER
                    pthread_mutex_lock(&debug_mtx);
                    fprintf(stderr, "\n[MANAGER] leggo PIPE: fd [%d] con valore [%d]\n", ans.fd, ans.conn_state);
                    pthread_mutex_unlock(&debug_mtx);
#endif
                    switch(ans.conn_state) {
                        case CONTINUA:
                            tmp.fd = ans.fd;
                            tmp.events = POLLIN;
                            MENO1(pollfd_add(&pollfd_v, tmp, &polled_fd))
#ifdef SHOW_INFO
                            pthread_mutex_lock(&debug_mtx);
                            fprintf(stderr, "\n[MANAGER] continuo su fd [%d]\n", ans.fd);
                            print_pollfd(pollfd_v, polled_fd);
                            pthread_mutex_unlock(&debug_mtx);
#endif

                            break;
                        case TERMINA:
#ifdef SHOW_INFO
                            pthread_mutex_lock(&debug_mtx);
                            cnt_open_files--;
                            fprintf(stderr, "\n[MANAGER] CLOSE su fd [%d], numero fd aperti = %d\n",\
                                    ans.fd, cnt_open_files);
                            puts("---------------------------------------------------------");
                            print_pollfd(pollfd_v, polled_fd);
                            pthread_mutex_unlock(&debug_mtx);
#endif
                            MENO1(close(ans.fd))            /* chiudo il descrittore terminato */
                            break;
                        default: // ERRORE (grave)
#ifdef SHOW_INFO_MANAGER
                            pthread_mutex_lock(&debug_mtx);
                            fprintf(stderr, "\n[MANAGER] CLOSE su fd [%d]\n", ans.fd);
                            pthread_mutex_unlock(&debug_mtx);
#endif
                            MENO1(close(ans.fd))            /* chiudo il descrittore terminato */
                           // fprintf(stderr, "[MANAGER] errore di comunicazione su fd []. Quitting\n", ans.fd);
                    }
                } else {                                /* richiesta di lavoro */
                    /*
                     * pollfd_v[i].fd è un fd di un socket di comunicazione
                     */
                    MENO1(pollfd_remove(pollfd_v, i, &polled_fd))
                    current_pollfd_array_size--;
#ifdef SHOW_POLLFD
                    pthread_mutex_lock(&debug_mtx);
                    printf("[MANAGER] accetta richiesta di lavoro di [%d]\n", fd_curr);
                    print_pollfd(pollfd_v, polled_fd);
                    pthread_mutex_unlock(&debug_mtx);
#endif
                    if(get_num_free_thread() > 0) {
                        /*
                         * - inserisco la richiesta nella coda FIFO (mutua esclusione garantita
                         *              dall'interfaccia)
                         */
#ifdef SHOW_INFO
                        pthread_mutex_lock(&debug_mtx);
                        fprintf(stderr, "\n[MANAGER] scrive sulla CODA fd [%d]\n", fd_curr);
                        pthread_mutex_unlock(&debug_mtx);
#endif
                        MENO1(insertFIFO(Q, (void *) fd_curr))
                    } else {
                        /*
                         * - spawn dinamico di thread in modalità detached (così da non doverli attendere)
                         */
                        while(spawn_thread(fd_curr) == -1) {
                            if(errno != EAGAIN) {
                                perror("spawn_thread");
                                exit(EXIT_FAILURE);
                            }
                        }
#ifdef SHOW_INFO
                        pthread_mutex_lock(&debug_mtx);
                        fprintf(stderr, "\n[MANAGER] spawna thread con fd [%d]\n", fd_curr);
                        pthread_mutex_unlock(&debug_mtx);
#endif
                    }
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
        MENO1(insertFIFO(Q, (void *) WORKER_EXIT_NUM))

    for(i=0; i < size_th_pool; i++) {
        PTH(err, pthread_join(tid_ws[i], NULL))
    }
    free(tid_ws);
    free(pollfd_v);

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
     *      - gestisce una singola richiesta di un client
     *      - alla terminazione della richiesta invia sulla
     *          pipe il fd del client unito allo stato della
     *          comunicazione
     *      - aggiorna il numero di thread liberi
     ********************************************************/
    for(;;) {
        if(get_termina_server() == 1)
            break;
        int fdc = (int) getFIFO(Q);                /* se la coda è vuota => chiamata bloccante */
        EQNULL((void *)fdc);
        if(fdc == (int) NULL)
            continue;
        if(fdc == WORKER_EXIT_NUM)
            return (void *) NULL;

        ch_num_free_thread(-1);
        manage_connection((void *)fdc);
        ch_num_free_thread(+1);

    }
    return (void *) NULL;
}

/**
 *
 * @INPUT   arg                 fd socket di comunicazione col client da servire
 * @EFFECTS soddisfa una richiesta di un client
 * @RETURNS r                   valore di ritorno della richiesta, indica lo stato
 *                              della comunicazione, tra TERMINA, CONINUA ed ERRORE
 */
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

#ifdef SHOW_INFO
    pthread_mutex_lock(&debug_mtx);
    fprintf(stderr, "\n[WORKER %d] connessione su fd [%d]\n", syscall(__NR_gettid), fdc);
    pthread_mutex_unlock(&debug_mtx);
#endif

    r = do_request(&buffer, fdc);
    free(buffer.str);
    /*
     * scrittura sulla pipe
     */
    elem_pipe_t tmp;
    tmp.fd = fdc;
    tmp.conn_state = r;
#ifdef SHOW_INFO
    pthread_mutex_lock(&debug_mtx);
    if(r == TERMINA) {
        fprintf(stderr, "\n[WORKER %d] TERMINA su fd [%d]\n", syscall(__NR_gettid), fdc);
    } else if (r == CONTINUA) {
        fprintf(stderr, "\n[WORKER %d] CONTINUA su fd [%d]\n", syscall(__NR_gettid), fdc);
    }
    pthread_mutex_unlock(&debug_mtx);
#endif
    MENO1(insert_pipe(tmp))

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
#ifdef DEBUG
        fprintf(stderr, "\n[SERVER] legge: %d\n", len_read);
#endif
    CONN_IO(len_read)
    if(len_read > b->len) {
        b->len = len_read;
        EQNULL(b->str = realloc(b->str, b->len * sizeof(char)))
    }
    CONN_IO(readn(fdc, b->str, len_read * sizeof(char)))
#ifdef DEBUG
    fprintf(stderr, "\n[SERVER] legge: %s\n", b->str);
#endif
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
#ifdef DEBUG
    fprintf(stderr, "\n[SERVER] scrive: %d\n", len_read);
#endif
    CONN_IO(writen(fdc, b->str, len_read * sizeof(char)))
#ifdef DEBUG
    fprintf(stderr, "\n[SERVER] scrive: %s\n", b->str);
#endif

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
        errno = EAGAIN;
        return -1;
    }
    if( (err = pthread_attr_setdetachstate(&th_attr, PTHREAD_CREATE_DETACHED)) != 0) {
        errno = EAGAIN;
        PTH(err, pthread_attr_destroy(&th_attr))
        return -1;
    }
    pthread_t tid_tmp;
    if( (err = pthread_create(&tid_tmp, &th_attr, manage_connection, (void *)fdc )) != 0) {
        errno = EFAULT;
        PTH(err, pthread_attr_destroy(&th_attr))
        return -1;
    }
#ifdef SHOW_INFO
    pthread_mutex_lock(&debug_mtx);
    fprintf(stderr, "\n[MANAGER] connessione su fd [%d] tramite thread SPAWNED\n", fdc);
    pthread_mutex_unlock(&debug_mtx);
#endif

    return 0;
}