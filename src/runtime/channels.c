#include "runtime.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// External scheduler reference
extern goo_scheduler_t* g_scheduler;

// Channel creation
goo_channel_t* goo_make_chan(size_t elem_size, size_t buffer_size) {
    goo_channel_t* ch = goo_alloc(sizeof(goo_channel_t));
    memset(ch, 0, sizeof(goo_channel_t));
    
    ch->elem_size = elem_size;
    ch->capacity = buffer_size;
    ch->pattern = GOO_CHANNEL_BASIC;
    ch->mutex = goo_mutex_new();
    ch->not_empty = goo_alloc(sizeof(goo_cond_t));
    ch->not_full = goo_alloc(sizeof(goo_cond_t));
    
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
    // Go parity: close(nil) and a second close() both panic rather than
    // silently no-op — a v1 program that hits either had a real logic bug
    // (double-close race, or closing before the make(chan...) succeeded).
    if (!ch) {
        goo_panic("close of nil channel");
    }

    goo_mutex_lock(ch->mutex);

    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        goo_panic("close of closed channel");
    }
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

    // Close only if not already closed: goo_chan_close now panics on a
    // double-close (Go parity, P3.1), but this call is an internal
    // implementation detail (wake any waiters before the channel's memory
    // goes away), not a user-visible close() — a program that already
    // called close(ch) itself must not crash when its channel is freed.
    if (!ch->closed) {
        goo_chan_close(ch);
    }
    
    if (ch->buffer) {
        goo_free(ch->buffer);
    }

    if (ch->rv_slot) {
        goo_free(ch->rv_slot);
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

// Go parity: send/receive on a NIL channel blocks forever (Go spec,
// "Channel types" / range clause) — it must never look like a closed
// channel (which reports ready with a zero value). Blocking here routes
// through goo_sched_block_begin, so the main-only shape (`var ch chan
// int; <-ch` with no goroutines) gets Go's "all goroutines are asleep -
// deadlock!" abort from the detector's instant check; with live
// goroutines it parks forever, which is the detector's documented
// structural gap (concurrency.c), same as any other blocked-main case.
// select is unaffected: goo_chan_try_send/try_recv keep their own nil
// guard reporting "not ready", matching Go's nil-case-never-fires rule.
static void goo_chan_nil_block(void) {
    goo_sched_block_begin();
    for (;;) {
        goo_platform_sleep_ns(1000000000);  // 1s; nothing can ever wake this.
    }
}

// Channel send operation
int goo_chan_send(goo_channel_t* ch, void* data) {
    if (!data) {
        return 0;  // Internal misuse guard; unreachable from generated code.
    }
    if (!ch) {
        goo_chan_nil_block();
    }
    
    goo_mutex_lock(ch->mutex);

    // Check if channel is closed. Go parity: sending on an already-closed
    // channel panics rather than failing silently (P3.1) — every closed
    // check in this function converts to the same panic, not just this one,
    // so a send can't "fail quietly" depending on whether it raced a
    // concurrent close() or found the channel already closed.
    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        goo_panic("send on closed channel");
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
            // Mark the running goroutine as waiting for send. NOTE: this
            // bookkeeping is per-thread-correct but the deadlock detector still
            // cannot observe it — a channel-blocked goroutine parks in the
            // pthread_cond_wait below, not in g_scheduler->ready_queue (which is
            // all the detector walks). Working channel-deadlock detection needs
            // a separate blocked-set and is a future milestone.
            goo_goroutine_t* self = goo_current_goroutine();
            if (self) {
                self->waiting_on_channel = ch;
                self->waiting_for_send = 1;
                self->state = GOO_GOROUTINE_BLOCKED;
            }

#ifdef GOO_PLATFORM_UNIX
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
            goo_sched_block_end();
#endif

            // Clear waiting state when unblocked.
            if (self) {
                self->waiting_on_channel = NULL;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_RUNNING;
            }
        }

        if (ch->closed) {
            // Closed by another goroutine while this send was blocked
            // waiting for buffer space — Go parity, same panic as the
            // immediate-closed check above.
            goo_mutex_unlock(ch->mutex);
            goo_panic("send on closed channel");
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
        // Unbuffered channel: rendezvous handoff. Park the value in the 1-slot
        // buffer, wake a receiver, then block until the receiver consumes it so
        // send returns only after the value has been delivered (Go semantics).
#ifdef GOO_PLATFORM_UNIX
        // Wait until the slot is free (a previous sender's value was consumed).
        while (ch->rv_full && !ch->closed) {
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
            goo_sched_block_end();
        }
        if (ch->closed) {
            // Closed by another goroutine while this send was blocked
            // waiting for the rendezvous slot to free up — same panic as
            // the buffered-path equivalent above.
            goo_mutex_unlock(ch->mutex);
            goo_panic("send on closed channel");
        }

        if (!ch->rv_slot) {
            ch->rv_slot = goo_alloc(ch->elem_size);
        }
        memcpy(ch->rv_slot, data, ch->elem_size);
        ch->rv_full = 1;

        // Wake a waiting receiver.
        pthread_cond_signal(&ch->not_empty->cond);

        // Block until the receiver has copied the value out.
        while (ch->rv_full && !ch->closed) {
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
            goo_sched_block_end();
        }

        // If the channel was closed before a receiver took the parked value,
        // the send never completed — Go parity, same panic as the other
        // closed-during-send cases above (a delivery that DID complete
        // still returns success below even if the channel was closed
        // immediately after, since the handoff itself already succeeded).
        if (ch->rv_full) {
            goo_mutex_unlock(ch->mutex);
            goo_panic("send on closed channel");
        }
        goo_mutex_unlock(ch->mutex);
        return 1;
#else
        goo_mutex_unlock(ch->mutex);
        return 0;
#endif
    }
}

// Channel receive operation
int goo_chan_recv(goo_channel_t* ch, void* data) {
    if (!data) {
        return 0;  // Internal misuse guard; unreachable from generated code.
    }
    if (!ch) {
        goo_chan_nil_block();
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
            // See the send-wait note above re: deadlock-detector visibility.
            goo_goroutine_t* self = goo_current_goroutine();
            if (self) {
                self->waiting_on_channel = ch;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_BLOCKED;
            }

#ifdef GOO_PLATFORM_UNIX
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);
            goo_sched_block_end();
#endif

            // Clear waiting state when unblocked.
            if (self) {
                self->waiting_on_channel = NULL;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_RUNNING;
            }
        }
        
        if (ch->length == 0 && ch->closed) {
            // Go's comma-ok zero-value contract: `v, ok := <-ch` on a
            // closed+drained channel must deliver ch's element zero value,
            // not whatever garbage happened to be on the caller's stack —
            // the caller (codegen's comma-ok path) always loads this buffer
            // regardless of the returned status.
            memset(data, 0, ch->elem_size);
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
        // Unbuffered channel: rendezvous handoff. Block until a sender has
        // parked a value, copy it out, clear the slot, and wake the sender.
#ifdef GOO_PLATFORM_UNIX
        while (!ch->rv_full && !ch->closed) {
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);
            goo_sched_block_end();
        }
        if (!ch->rv_full && ch->closed) {
            // Same zero-value contract as the buffered path above.
            memset(data, 0, ch->elem_size);
            goo_mutex_unlock(ch->mutex);
            return 0;  // Closed with no value parked.
        }

        memcpy(data, ch->rv_slot, ch->elem_size);
        ch->rv_full = 0;

        // Release the sender (its "wait until consumed" loop) and any sender
        // waiting for the slot to free up.
        pthread_cond_signal(&ch->not_full->cond);

        goo_mutex_unlock(ch->mutex);
        return 1;
#else
        goo_mutex_unlock(ch->mutex);
        return 0;
#endif
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

    // Go parity: try-send on a closed channel panics, same as blocking
    // send's closed check (goo_chan_send above) — a non-blocking send is
    // still a send.
    if (ch->closed) {
        goo_mutex_unlock(ch->mutex);
        goo_panic("send on closed channel");
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

    // Go select semantics: a receive on a CLOSED channel is immediately
    // READY and yields the element zero value — checked only after the
    // buffered-data branch above, so pending values drain before the
    // closed state reports ready (Go's order). Without this, goo_select's
    // poll loop treated closed+drained as not-ready and busy-spun forever
    // (unreachable before close() shipped in P3.1, a user-visible hang
    // after). The comma-ok distinction is not lost: select's `v, ok :=`
    // form is rejected at typecheck in v1, and every other consumer goes
    // through goo_chan_recv, which signals closed via its status return.
    if (ch->closed) {
        memset(data, 0, ch->elem_size);
        goo_mutex_unlock(ch->mutex);
        return 1;  // Ready: zero value from a closed channel.
    }

    // Nothing buffered and not closed — a plain would-block. The
    // zero-value memset keeps the out-buffer deterministic for callers
    // that load it regardless of status (see goo_chan_recv above).
    memset(data, 0, ch->elem_size);
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

// Timeout operations (simplified implementation)
int goo_chan_send_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns) {
    // For now, just try non-blocking operation
    // TODO: Implement proper timeout with timer
    (void)timeout_ns;
    return goo_chan_try_send(ch, data);
}

int goo_chan_recv_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns) {
    // For now, just try non-blocking operation
    // TODO: Implement proper timeout with timer
    (void)timeout_ns;
    return goo_chan_try_recv(ch, data);
}

// Select operation.
//
// Scans the cases (skipping inactive slots whose channel is NULL — these are
// placeholders for the `default` case) and returns the index of the first ready
// case, or -1 if none. The timeout_ns argument encodes the blocking policy:
//   timeout_ns == 0  : non-blocking (a `default` case is present) — try once and
//                       return -1 if nothing is ready so the default fires.
//   timeout_ns <  0  : block until some case becomes ready (no default).
//   timeout_ns >  0  : block until a case is ready or the deadline passes (-1).
//
// Blocking is implemented by polling, matching the scheduler's existing
// sleep-based idiom. A condvar-based wakeup is a future optimization (M9).
int goo_select(goo_select_case_t* cases, size_t num_cases, int64_t timeout_ns) {
    if (!cases || num_cases == 0) {
        return -1;  // Only a default (or nothing) — caller fires the default.
    }

    uint64_t deadline = 0;
    if (timeout_ns > 0) {
        deadline = goo_platform_time_ns() + (uint64_t)timeout_ns;
    }

    for (;;) {
        for (size_t i = 0; i < num_cases; i++) {
            goo_select_case_t* case_ptr = &cases[i];
            case_ptr->ready = 0;

            if (!case_ptr->channel) {
                continue;  // Inactive slot (default placeholder).
            }

            int ok = case_ptr->is_send
                         ? goo_chan_try_send(case_ptr->channel, case_ptr->data)
                         : goo_chan_try_recv(case_ptr->channel, case_ptr->data);
            if (ok) {
                case_ptr->ready = 1;
                return (int)i;
            }
        }

        if (timeout_ns == 0) {
            return -1;  // Non-blocking: nothing ready, fire the default.
        }
        if (timeout_ns > 0 && goo_platform_time_ns() >= deadline) {
            return -1;  // Timed out.
        }

        goo_platform_sleep_ns(200000);  // 0.2ms between polls.
    }
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