#include "runtime.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// External scheduler reference
extern goo_scheduler_t* g_scheduler;

// Channel creation
goo_channel_t* goo_make_chan(size_t elem_size, size_t buffer_size) {
    goo_channel_t* ch = goo_alloc(sizeof(goo_channel_t));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(goo_channel_t));

    ch->elem_size = elem_size;
    ch->capacity = buffer_size;
    ch->pattern = GOO_CHANNEL_BASIC;
    ch->mutex = goo_mutex_new();
    ch->not_empty = goo_alloc(sizeof(goo_cond_t));
    ch->not_full = goo_alloc(sizeof(goo_cond_t));
    if (!ch->mutex || !ch->not_empty || !ch->not_full) {
        if (ch->not_full) goo_free(ch->not_full);
        if (ch->not_empty) goo_free(ch->not_empty);
        if (ch->mutex) goo_mutex_free(ch->mutex);
        goo_free(ch);
        return NULL;
    }
    
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_init(&ch->not_empty->cond, NULL);
    pthread_cond_init(&ch->not_full->cond, NULL);
#endif
    
    if (buffer_size > 0) {
        ch->buffer = goo_alloc(elem_size * buffer_size);
    }
    
    if (g_scheduler) {
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        ch->id = g_scheduler->next_channel_id++;
        g_scheduler->stats.num_channels++;
        goo_mutex_unlock(g_scheduler->scheduler_mutex);
    }
    
    return ch;
}

goo_channel_t* goo_make_pattern_chan(goo_channel_pattern_t pattern, size_t elem_size, const char* endpoint) {
    goo_channel_t* ch = goo_make_chan(elem_size, 0);  // Pattern channels are usually unbuffered
    ch->pattern = pattern;
    
    if (endpoint) {
        size_t len = strlen(endpoint);
        ch->endpoint = goo_alloc(len + 1);
        strcpy(ch->endpoint, endpoint);
    }
    
    // Initialize pattern-specific data
    switch (pattern) {
        case GOO_CHANNEL_PUB:
            ch->pattern_data.pub_data.subscribers = NULL;
            ch->pattern_data.pub_data.subscriber_count = 0;
            break;
        case GOO_CHANNEL_SUB:
            ch->pattern_data.sub_data.publisher = NULL;
            ch->pattern_data.sub_data.sub_node = NULL;
            break;
        case GOO_CHANNEL_REQ:
        case GOO_CHANNEL_REP:
            ch->pattern_data.req_rep_data.paired_channel = NULL;
            ch->pattern_data.req_rep_data.request_id_counter = 0;
            break;
        case GOO_CHANNEL_PUSH:
        case GOO_CHANNEL_PULL:
            ch->pattern_data.push_pull_data.workers = NULL;
            ch->pattern_data.push_pull_data.worker_count = 0;
            ch->pattern_data.push_pull_data.next_worker_index = 0;
            break;
        default:
            break;
    }
    
    return ch;
}

void goo_chan_close(goo_channel_t* ch) {
    if (!ch) return;
    
    goo_mutex_lock(ch->mutex);
    ch->closed = 1;
    
    // Wake up all waiting goroutines
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_broadcast(&ch->not_empty->cond);
    pthread_cond_broadcast(&ch->not_full->cond);
#endif
    
    goo_mutex_unlock(ch->mutex);
}

void goo_chan_free(goo_channel_t* ch) {
    if (!ch) return;
    
    goo_chan_close(ch);
    
    if (ch->buffer) {
        goo_free(ch->buffer);
    }
    
    if (ch->endpoint) {
        goo_free(ch->endpoint);
    }
    
    // Clean up pattern-specific data
    switch (ch->pattern) {
        case GOO_CHANNEL_PUB:
            {
                struct goo_subscriber* sub = ch->pattern_data.pub_data.subscribers;
                while (sub) {
                    struct goo_subscriber* next = sub->next;
                    goo_free(sub);
                    sub = next;
                }
            }
            break;
        case GOO_CHANNEL_PUSH:
            if (ch->pattern_data.push_pull_data.workers) {
                goo_free(ch->pattern_data.push_pull_data.workers);
            }
            break;
        default:
            break;
    }
    
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_destroy(&ch->not_empty->cond);
    pthread_cond_destroy(&ch->not_full->cond);
#endif
    
    goo_free(ch->not_empty);
    goo_free(ch->not_full);
    goo_mutex_free(ch->mutex);
    
    if (g_scheduler) {
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        g_scheduler->stats.num_channels--;
        goo_mutex_unlock(g_scheduler->scheduler_mutex);
    }
    
    goo_free(ch);
}

// Channel send operation
int goo_chan_send(goo_channel_t* ch, void* data) {
    if (!ch || !data) {
        return 0;  // Failed
    }
    
    goo_mutex_lock(ch->mutex);
    
    // Check if channel is closed
    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        return 0;  // Failed - channel closed
    }
    
    // Handle different channel patterns
    switch (ch->pattern) {
        case GOO_CHANNEL_BASIC:
            break;  // Standard Go-style channel
            
        case GOO_CHANNEL_PUB:
            // Publisher channel - broadcast to all subscribers
            {
                struct goo_subscriber* sub = ch->pattern_data.pub_data.subscribers;
                int sent_count = 0;
                
                while (sub) {
                    if (sub->active && sub->channel && !goo_chan_is_closed(sub->channel)) {
                        // Try to send to this subscriber
                        if (goo_chan_try_send(sub->channel, data)) {
                            sent_count++;
                        }
                    }
                    sub = sub->next;
                }
                
                goo_mutex_unlock(ch->mutex);
                return sent_count > 0 ? 1 : 0;  // Success if at least one subscriber received
            }
            
        case GOO_CHANNEL_REQ:
            // Request channel - paired with reply channel
            {
                goo_channel_t* rep_chan = ch->pattern_data.req_rep_data.paired_channel;
                if (!rep_chan) {
                    goo_mutex_unlock(ch->mutex);
                    return 0;  // No paired reply channel
                }
                
                // For now, just send the request normally
                // TODO: Add request ID correlation for proper req/rep
                goo_mutex_unlock(ch->mutex);
                return goo_chan_send(rep_chan, data);
            }
            
        case GOO_CHANNEL_PUSH:
            // Push channel - load balancing to workers
            {
                if (ch->pattern_data.push_pull_data.worker_count == 0) {
                    goo_mutex_unlock(ch->mutex);
                    return 0;  // No workers available
                }
                
                // Round-robin to next worker
                size_t worker_index = ch->pattern_data.push_pull_data.next_worker_index;
                goo_channel_t* worker = ch->pattern_data.push_pull_data.workers[worker_index];
                
                ch->pattern_data.push_pull_data.next_worker_index = 
                    (worker_index + 1) % ch->pattern_data.push_pull_data.worker_count;
                
                goo_mutex_unlock(ch->mutex);
                
                // Send directly to the selected worker (avoid recursion)
                return goo_chan_try_send(worker, data);
            }
            
        case GOO_CHANNEL_SUB:
        case GOO_CHANNEL_REP:
        case GOO_CHANNEL_PULL:
            // These patterns accept normal sends
            break;
            
        default:
            goo_mutex_unlock(ch->mutex);
            return 0;  // Unsupported pattern
    }
    
    // For buffered channels
    if (ch->capacity > 0) {
        // Wait for space in buffer
        while (ch->length >= ch->capacity && !ch->closed) {
            // Mark current goroutine as waiting for send
            if (g_scheduler && g_scheduler->current_goroutine) {
                g_scheduler->current_goroutine->waiting_on_channel = ch;
                g_scheduler->current_goroutine->waiting_for_send = 1;
                g_scheduler->current_goroutine->state = GOO_GOROUTINE_BLOCKED;
            }
            
#ifdef GOO_PLATFORM_UNIX
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
#endif

            // Clear waiting state when unblocked
            if (g_scheduler && g_scheduler->current_goroutine) {
                g_scheduler->current_goroutine->waiting_on_channel = NULL;
                g_scheduler->current_goroutine->waiting_for_send = 0;
                g_scheduler->current_goroutine->state = GOO_GOROUTINE_RUNNING;
            }
        }
        
        if (ch->closed) {
            goo_mutex_unlock(ch->mutex);
            return 0;
        }
        
        // Copy data to buffer
        void* dest = (char*)ch->buffer + (ch->tail * ch->elem_size);
        memcpy(dest, data, ch->elem_size);
        
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->length++;
        
        // Signal receivers
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_signal(&ch->not_empty->cond);
#endif
        
        goo_mutex_unlock(ch->mutex);
        return 1;  // Success
    } else {
        // Unbuffered channel - rendezvous handoff
        // Check if a receiver is already waiting
        if (ch->recv_waiters) {
            // A receiver is waiting — copy data directly and wake it
            // The receiver stored its destination pointer in recv_waiters->arg
            goo_goroutine_t* receiver = ch->recv_waiters;
            ch->recv_waiters = receiver->next;
            receiver->next = NULL;

            if (receiver->arg) {
                memcpy(receiver->arg, data, ch->elem_size);
            }

            // Wake the receiver
            receiver->waiting_on_channel = NULL;
            receiver->state = GOO_GOROUTINE_READY;
#ifdef GOO_PLATFORM_UNIX
            pthread_cond_signal(&ch->not_empty->cond);
#endif

            goo_mutex_unlock(ch->mutex);
            return 1;
        }

        // No receiver waiting — block until one arrives
        // Store our data pointer so the receiver can copy from it
        goo_goroutine_t* self = g_scheduler ? g_scheduler->current_goroutine : NULL;
        if (self) {
            self->arg = data;
            self->waiting_on_channel = ch;
            self->waiting_for_send = 1;
            self->state = GOO_GOROUTINE_BLOCKED;
            self->next = ch->send_waiters;
            ch->send_waiters = self;
        }

#ifdef GOO_PLATFORM_UNIX
        while (!ch->closed && self && self->state == GOO_GOROUTINE_BLOCKED) {
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
        }
#endif

        if (self) {
            self->waiting_on_channel = NULL;
            self->waiting_for_send = 0;
            self->state = GOO_GOROUTINE_RUNNING;
        }

        int success = !ch->closed;
        goo_mutex_unlock(ch->mutex);
        return success;
    }
}

// Channel receive operation
int goo_chan_recv(goo_channel_t* ch, void* data) {
    if (!ch || !data) {
        return 0;  // Failed
    }
    
    goo_mutex_lock(ch->mutex);
    
    // Handle different channel patterns
    switch (ch->pattern) {
        case GOO_CHANNEL_BASIC:
            break;  // Standard Go-style channel
            
        case GOO_CHANNEL_SUB:
            // Subscriber channel - receive from publisher
            // This is handled by the publisher broadcasting to this channel
            // Just do regular channel receive
            break;
            
        case GOO_CHANNEL_REP:
            // Reply channel - paired with request channel
            // This receives requests from the paired REQ channel
            // Just do regular channel receive
            break;
            
        case GOO_CHANNEL_PULL:
            // Pull channel - receive from push
            // This is handled by the push channel sending to this pull channel
            // Just do regular channel receive
            break;
            
        default:
            goo_mutex_unlock(ch->mutex);
            return 0;  // Unsupported pattern
    }
    
    // For buffered channels
    if (ch->capacity > 0) {
        // Wait for data in buffer
        while (ch->length == 0 && !ch->closed) {
            // Mark current goroutine as waiting for receive
            if (g_scheduler && g_scheduler->current_goroutine) {
                g_scheduler->current_goroutine->waiting_on_channel = ch;
                g_scheduler->current_goroutine->waiting_for_send = 0;
                g_scheduler->current_goroutine->state = GOO_GOROUTINE_BLOCKED;
            }
            
#ifdef GOO_PLATFORM_UNIX
            pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);
#endif

            // Clear waiting state when unblocked
            if (g_scheduler && g_scheduler->current_goroutine) {
                g_scheduler->current_goroutine->waiting_on_channel = NULL;
                g_scheduler->current_goroutine->waiting_for_send = 0;
                g_scheduler->current_goroutine->state = GOO_GOROUTINE_RUNNING;
            }
        }
        
        if (ch->length == 0 && ch->closed) {
            goo_mutex_unlock(ch->mutex);
            return 0;  // No more data and channel closed
        }
        
        // Copy data from buffer
        void* src = (char*)ch->buffer + (ch->head * ch->elem_size);
        memcpy(data, src, ch->elem_size);
        
        ch->head = (ch->head + 1) % ch->capacity;
        ch->length--;
        
        // Signal senders
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_signal(&ch->not_full->cond);
#endif
        
        goo_mutex_unlock(ch->mutex);
        return 1;  // Success
    } else {
        // Unbuffered channel - rendezvous handoff
        // Check if a sender is already waiting
        if (ch->send_waiters) {
            // A sender is waiting — copy data directly from its buffer
            goo_goroutine_t* sender = ch->send_waiters;
            ch->send_waiters = sender->next;
            sender->next = NULL;

            if (sender->arg) {
                memcpy(data, sender->arg, ch->elem_size);
            }

            // Wake the sender
            sender->waiting_on_channel = NULL;
            sender->waiting_for_send = 0;
            sender->state = GOO_GOROUTINE_READY;
#ifdef GOO_PLATFORM_UNIX
            pthread_cond_signal(&ch->not_full->cond);
#endif

            goo_mutex_unlock(ch->mutex);
            return 1;
        }

        // Closed channel with no senders — fail
        if (ch->closed) {
            goo_mutex_unlock(ch->mutex);
            return 0;
        }

        // No sender waiting — block until one arrives
        // Store our destination pointer so the sender can copy into it
        goo_goroutine_t* self = g_scheduler ? g_scheduler->current_goroutine : NULL;
        if (self) {
            self->arg = data;
            self->waiting_on_channel = ch;
            self->waiting_for_send = 0;
            self->state = GOO_GOROUTINE_BLOCKED;
            self->next = ch->recv_waiters;
            ch->recv_waiters = self;
        }

#ifdef GOO_PLATFORM_UNIX
        while (!ch->closed && self && self->state == GOO_GOROUTINE_BLOCKED) {
            pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);
        }
#endif

        if (self) {
            self->waiting_on_channel = NULL;
            self->state = GOO_GOROUTINE_RUNNING;
        }

        int success = !ch->closed;
        goo_mutex_unlock(ch->mutex);
        return success;
    }
}

// Non-blocking operations
int goo_chan_try_send(goo_channel_t* ch, void* data) {
    if (!ch || !data) {
        return 0;
    }
    
    if (!goo_mutex_try_lock(ch->mutex)) {
        return 0;  // Could not acquire lock
    }
    
    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        return 0;
    }
    
    if (ch->capacity > 0 && ch->length < ch->capacity) {
        // Space available in buffer
        void* dest = (char*)ch->buffer + (ch->tail * ch->elem_size);
        memcpy(dest, data, ch->elem_size);
        
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->length++;
        
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_signal(&ch->not_empty->cond);
#endif
        
        goo_mutex_unlock(ch->mutex);
        return 1;  // Success
    }
    
    goo_mutex_unlock(ch->mutex);
    return 0;  // Would block
}

int goo_chan_try_recv(goo_channel_t* ch, void* data) {
    if (!ch || !data) {
        return 0;
    }
    
    if (!goo_mutex_try_lock(ch->mutex)) {
        return 0;  // Could not acquire lock
    }
    
    if (ch->capacity > 0 && ch->length > 0) {
        // Data available in buffer
        void* src = (char*)ch->buffer + (ch->head * ch->elem_size);
        memcpy(data, src, ch->elem_size);
        
        ch->head = (ch->head + 1) % ch->capacity;
        ch->length--;
        
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_signal(&ch->not_full->cond);
#endif
        
        goo_mutex_unlock(ch->mutex);
        return 1;  // Success
    }
    
    goo_mutex_unlock(ch->mutex);
    return 0;  // Would block
}

// Channel state queries
int goo_chan_is_closed(goo_channel_t* ch) {
    if (!ch) return 1;
    
    goo_mutex_lock(ch->mutex);
    int closed = ch->closed;
    goo_mutex_unlock(ch->mutex);
    
    return closed;
}

size_t goo_chan_len(goo_channel_t* ch) {
    if (!ch) return 0;
    
    goo_mutex_lock(ch->mutex);
    size_t length = ch->length;
    goo_mutex_unlock(ch->mutex);
    
    return length;
}

size_t goo_chan_cap(goo_channel_t* ch) {
    if (!ch) return 0;
    
    return ch->capacity;
}

// Timeout operations
int goo_chan_send_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns) {
    if (!ch || !data) return 0;

    // Try non-blocking first
    if (goo_chan_try_send(ch, data)) return 1;
    if (timeout_ns == 0) return 0;

#ifdef GOO_PLATFORM_UNIX
    goo_mutex_lock(ch->mutex);

    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        return 0;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ns / 1000000000ULL;
    deadline.tv_nsec += timeout_ns % 1000000000ULL;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    // Wait for space (buffered) or receiver (unbuffered)
    while ((ch->capacity > 0 ? ch->length >= ch->capacity : !ch->recv_waiters) && !ch->closed) {
        int rc = pthread_cond_timedwait(&ch->not_full->cond, &ch->mutex->mutex, &deadline);
        if (rc != 0) {
            goo_mutex_unlock(ch->mutex);
            return 0;  // Timeout
        }
    }

    goo_mutex_unlock(ch->mutex);

    // Try the actual send now that there may be space/receiver
    return goo_chan_try_send(ch, data) ? 1 : goo_chan_send(ch, data);
#else
    return goo_chan_try_send(ch, data);
#endif
}

int goo_chan_recv_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns) {
    if (!ch || !data) return 0;

    // Try non-blocking first
    if (goo_chan_try_recv(ch, data)) return 1;
    if (timeout_ns == 0) return 0;

#ifdef GOO_PLATFORM_UNIX
    goo_mutex_lock(ch->mutex);

    if (ch->closed && ch->length == 0) {
        goo_mutex_unlock(ch->mutex);
        return 0;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ns / 1000000000ULL;
    deadline.tv_nsec += timeout_ns % 1000000000ULL;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    // Wait for data (buffered) or sender (unbuffered)
    while ((ch->capacity > 0 ? ch->length == 0 : !ch->send_waiters) && !ch->closed) {
        int rc = pthread_cond_timedwait(&ch->not_empty->cond, &ch->mutex->mutex, &deadline);
        if (rc != 0) {
            goo_mutex_unlock(ch->mutex);
            return 0;  // Timeout
        }
    }

    goo_mutex_unlock(ch->mutex);

    // Try the actual recv now that there may be data/sender
    return goo_chan_try_recv(ch, data) ? 1 : goo_chan_recv(ch, data);
#else
    return goo_chan_try_recv(ch, data);
#endif
}

// Select operation — polls cases, then blocks with timeout if none ready
int goo_select(goo_select_case_t* cases, size_t num_cases, int64_t timeout_ns) {
    if (!cases || num_cases == 0) {
        return -1;
    }

    // Phase 1: Non-blocking poll of all cases
    for (size_t i = 0; i < num_cases; i++) {
        goo_select_case_t* c = &cases[i];
        c->ready = 0;

        if (c->is_send) {
            if (goo_chan_try_send(c->channel, c->data)) {
                c->ready = 1;
                return (int)i;
            }
        } else {
            if (goo_chan_try_recv(c->channel, c->data)) {
                c->ready = 1;
                return (int)i;
            }
        }
    }

    // If timeout is 0, non-blocking only
    if (timeout_ns == 0) {
        return -1;
    }

#ifdef GOO_PLATFORM_UNIX
    // Phase 2: Block with polling loop and timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (timeout_ns > 0) {
        deadline.tv_sec += timeout_ns / 1000000000LL;
        deadline.tv_nsec += timeout_ns % 1000000000LL;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    // Use the first case's channel condition variable for waiting
    // This is a simplified approach — a full implementation would
    // register with all channels for wakeup
    goo_channel_t* wait_ch = cases[0].channel;
    if (!wait_ch) return -1;

    goo_mutex_lock(wait_ch->mutex);

    while (1) {
        // Try all cases again under lock
        goo_mutex_unlock(wait_ch->mutex);

        for (size_t i = 0; i < num_cases; i++) {
            goo_select_case_t* c = &cases[i];
            if (c->is_send) {
                if (goo_chan_try_send(c->channel, c->data)) {
                    c->ready = 1;
                    return (int)i;
                }
            } else {
                if (goo_chan_try_recv(c->channel, c->data)) {
                    c->ready = 1;
                    return (int)i;
                }
            }
        }

        goo_mutex_lock(wait_ch->mutex);

        // Check timeout
        if (timeout_ns > 0) {
            int rc = pthread_cond_timedwait(&wait_ch->not_empty->cond,
                                            &wait_ch->mutex->mutex, &deadline);
            if (rc != 0) {
                // Timeout or error
                goo_mutex_unlock(wait_ch->mutex);
                return -1;
            }
        } else {
            // Block indefinitely (timeout_ns < 0)
            pthread_cond_wait(&wait_ch->not_empty->cond, &wait_ch->mutex->mutex);
        }
    }
#else
    return -1;
#endif
}

// Channel pattern management functions

int goo_chan_subscribe(goo_channel_t* publisher, goo_channel_t* subscriber) {
    if (!publisher || !subscriber) return 0;
    if (publisher->pattern != GOO_CHANNEL_PUB || subscriber->pattern != GOO_CHANNEL_SUB) {
        return 0;  // Invalid pattern types
    }
    
    goo_mutex_lock(publisher->mutex);
    
    // Check if already subscribed
    struct goo_subscriber* current = publisher->pattern_data.pub_data.subscribers;
    while (current) {
        if (current->channel == subscriber) {
            current->active = 1;  // Reactivate if was inactive
            goo_mutex_unlock(publisher->mutex);
            return 1;
        }
        current = current->next;
    }
    
    // Add new subscriber
    struct goo_subscriber* new_sub = goo_alloc(sizeof(struct goo_subscriber));
    if (!new_sub) {
        goo_mutex_unlock(publisher->mutex);
        return 0;
    }
    
    new_sub->channel = subscriber;
    new_sub->active = 1;
    new_sub->next = publisher->pattern_data.pub_data.subscribers;
    publisher->pattern_data.pub_data.subscribers = new_sub;
    publisher->pattern_data.pub_data.subscriber_count++;
    
    // Set up back-reference in subscriber
    goo_mutex_lock(subscriber->mutex);
    subscriber->pattern_data.sub_data.publisher = publisher;
    subscriber->pattern_data.sub_data.sub_node = new_sub;
    goo_mutex_unlock(subscriber->mutex);
    
    goo_mutex_unlock(publisher->mutex);
    return 1;
}

int goo_chan_unsubscribe(goo_channel_t* publisher, goo_channel_t* subscriber) {
    if (!publisher || !subscriber) return 0;
    if (publisher->pattern != GOO_CHANNEL_PUB || subscriber->pattern != GOO_CHANNEL_SUB) {
        return 0;  // Invalid pattern types
    }
    
    goo_mutex_lock(publisher->mutex);
    
    struct goo_subscriber** current = &publisher->pattern_data.pub_data.subscribers;
    while (*current) {
        if ((*current)->channel == subscriber) {
            struct goo_subscriber* to_remove = *current;
            *current = (*current)->next;
            publisher->pattern_data.pub_data.subscriber_count--;
            
            // Clear back-reference in subscriber
            goo_mutex_lock(subscriber->mutex);
            subscriber->pattern_data.sub_data.publisher = NULL;
            subscriber->pattern_data.sub_data.sub_node = NULL;
            goo_mutex_unlock(subscriber->mutex);
            
            goo_free(to_remove);
            goo_mutex_unlock(publisher->mutex);
            return 1;
        }
        current = &(*current)->next;
    }
    
    goo_mutex_unlock(publisher->mutex);
    return 0;  // Not found
}

int goo_chan_pair_req_rep(goo_channel_t* req_chan, goo_channel_t* rep_chan) {
    if (!req_chan || !rep_chan) return 0;
    if (req_chan->pattern != GOO_CHANNEL_REQ || rep_chan->pattern != GOO_CHANNEL_REP) {
        return 0;  // Invalid pattern types
    }
    
    goo_mutex_lock(req_chan->mutex);
    goo_mutex_lock(rep_chan->mutex);
    
    req_chan->pattern_data.req_rep_data.paired_channel = rep_chan;
    rep_chan->pattern_data.req_rep_data.paired_channel = req_chan;
    
    req_chan->pattern_data.req_rep_data.request_id_counter = 0;
    rep_chan->pattern_data.req_rep_data.request_id_counter = 0;
    
    goo_mutex_unlock(rep_chan->mutex);
    goo_mutex_unlock(req_chan->mutex);
    return 1;
}

int goo_chan_add_worker(goo_channel_t* push_chan, goo_channel_t* pull_chan) {
    if (!push_chan || !pull_chan) return 0;
    if (push_chan->pattern != GOO_CHANNEL_PUSH || pull_chan->pattern != GOO_CHANNEL_PULL) {
        return 0;  // Invalid pattern types
    }
    
    goo_mutex_lock(push_chan->mutex);
    
    // Resize worker array if needed
    size_t new_count = push_chan->pattern_data.push_pull_data.worker_count + 1;
    goo_channel_t** new_workers = goo_realloc(
        push_chan->pattern_data.push_pull_data.workers,
        new_count * sizeof(goo_channel_t*)
    );
    
    if (!new_workers) {
        goo_mutex_unlock(push_chan->mutex);
        return 0;
    }
    
    new_workers[new_count - 1] = pull_chan;
    push_chan->pattern_data.push_pull_data.workers = new_workers;
    push_chan->pattern_data.push_pull_data.worker_count = new_count;
    
    // Set up back-reference in pull channel
    goo_mutex_lock(pull_chan->mutex);
    pull_chan->pattern_data.push_pull_data.workers = &push_chan;  // Single reference to push channel
    pull_chan->pattern_data.push_pull_data.worker_count = 1;
    goo_mutex_unlock(pull_chan->mutex);
    
    goo_mutex_unlock(push_chan->mutex);
    return 1;
}