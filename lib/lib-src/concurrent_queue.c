/*
 * CODA FIFO CONCORRENTE, unbounded, produttori-consumatori
 * politica:
*
 * Mutua esclusione sia fra consumatori che fra produttori
 */
#include <concurrent_queue.h>

int start_queue(queue_t **Q) {
    *Q = calloc(1, sizeof(queue_t));
    if(*Q == NULL) {
        // fprintf(stderr, "CALLOC fallita: %s\n", __func__);
        return -1;
    }
    (*Q)->nelems = 0;
    (*Q)->tail = NULL;
    (*Q)->head = NULL;
    MENO1( pthread_mutex_init (&((*Q)->mtx), NULL))
    MENO1( pthread_cond_init  (&((*Q)->cond_read), NULL))

    return 0;
}

/*
 * @INPUT       Q               coda != NULL
 *              new_elem        elemento da inserire nella coda, dato generico
 * @EFFECTS     inserisce new_elem nella coda Q
 * @RETURNS     0               successo
 *              -1              errore nell'allocazione dinamica di memoria
 */
static int insert_into_queue(queue_t *Q, void *new_elem) {
    node_t *new_node = calloc(1, sizeof(node_t));
    if(new_node == NULL) {
        return -1;
    }
    new_node->elem = new_elem;
    new_node->next = NULL;
    new_node->prec  = Q->tail;

    if(Q->head == NULL){        // non ci sono nodi nella coda
        Q->head = (Q->tail = new_node);
    } else {                    // inserisco in fondo, FIFO
        Q->tail->next   = new_node;
        Q->tail = new_node;
    }
    Q->nelems++;
    return 0;
}

/*
 * @INPUT       Q               coda != NULL
 * @EFFECTS     ritorna l'elemento in testa alla coda, estraendolo dalla struttura node_t.
 *              Dealloca il nodo in testa e restituisce il valore che conteneva
 * @RETURNS     val             valore del nodo in testa
 *              NULL            coda vuota
 */
static void *get_from_queue(queue_t *Q) {
    if(Q->nelems == 0) {
        return NULL;
    }
    node_t *node = Q->head;
    if(Q->nelems == 1) { // head=tail
        Q->head = (Q->tail = NULL);
    } else {
        Q->head = Q->head->next;
        Q->head->prec = NULL;
    }

    void *val = node->elem;
    free(node);
    Q->nelems--;
    return val;
}

/*
 * lock(mutex)
 * while(queue.isEmpty()
 *      wait(cond, mutex)
 * queue.get()
 * IF errori
 *      riporta errore
 * unlock(mutex)
 */
void *getFIFO(queue_t *Q) {
    QUEUENULL(Q, NULL)
    int r;          // retval, per errori
    void *val;

    PTH(r, pthread_mutex_lock(&(Q->mtx)))

    while(Q->nelems == 0)
       PTH(r, pthread_cond_wait(&(Q->cond_read), &(Q->mtx)))

    if( (val = get_from_queue(Q)) == NULL) {
        // fprintf(stderr, "NO elements in Queue: %s\n", __func__);
    }

    PTH(r, pthread_mutex_unlock(&(Q->mtx)))

    return val;
}

/*
 *      lock(mutex)
        queue.insert()
        IF(error during writing)
            unlock(mutex)
            return err
        signal(cond)
        unlock(mutex)
 */
int insertFIFO(queue_t *Q, void *new_elem) {
    QUEUENULL(Q, -1)
    int r;          // retval, per errori

    PTH(r, pthread_mutex_lock(&(Q->mtx)))

    if(insert_into_queue(Q, new_elem) == -1) {
        PTH(r, pthread_mutex_unlock(&(Q->mtx)))
        // fprintf(stderr, "CALLOC fallita: %s\n", __func__);
        return -1;
    }
    // print_queue_int(Q);
    PTH(r, pthread_cond_signal(&(Q->cond_read)))
    PTH(r, pthread_mutex_unlock(&(Q->mtx)))

    return 0;
}

void free_queue(queue_t *Q, enum deallocazione_t opt) {
    QUEUENULL(Q, )
    int r;
    PTH(r, pthread_mutex_lock(&(Q->mtx)))

    node_t  *curr = Q->head,
            *curr_prev;
    while(curr != NULL) {
        curr_prev = curr;
        curr = curr->next;
        if(opt == DYNAMIC_ELEMS)
            free(curr_prev->elem);
        free(curr_prev);
    }
    if(&(Q->cond_read) != NULL) {
        PTH(r, pthread_cond_destroy(&(Q->cond_read)))
    }
    PTH(r, pthread_mutex_unlock(&(Q->mtx)))

    if(&(Q->mtx) != NULL) {
        PTH(r, pthread_mutex_destroy(&(Q->mtx)))
    }
    free(Q);
}

void print_queue_int(queue_t *Q) {
    QUEUENULL(Q, )
    int r;
    // PTH(r, pthread_mutex_lock(&(Q->mtx)))
    puts("\nCODA:");
    node_t *tmp = Q->head;
    while(tmp != NULL) {
        printf("%d\n", (int)tmp->elem);
        tmp = tmp->next;
    }

    // PTH(r, pthread_mutex_unlock(&(Q->mtx)))
}