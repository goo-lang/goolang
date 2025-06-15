#include "../../include/fearless_concurrency.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>

// Thread-local storage for current actor
static __thread Actor* current_actor = NULL;

// Utility functions
static uint64_t get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = ATOMIC_VAR_INIT(1);
    return atomic_fetch_add(&counter, 1);
}

// Default configuration presets
ActorSystemConfig actor_system_default_config(void) {
    return (ActorSystemConfig){
        .max_actors = 10000,
        .thread_pool_size = 8,
        .message_queue_size = 1000,
        .max_messages_per_actor = 100,
        
        .numa_aware = false,
        .work_stealing_enabled = true,
        .work_steal_batch_size = 4,
        
        .enable_actor_monitoring = true,
        .enable_deadlock_detection = true,
        .enable_performance_tracking = true,
        
        .actor_memory_pool_size = 1024 * 1024,     // 1MB
        .message_memory_pool_size = 2 * 1024 * 1024, // 2MB
        .use_memory_pools = true
    };
}

ActorSystemConfig actor_system_performance_config(void) {
    ActorSystemConfig config = actor_system_default_config();
    config.thread_pool_size = 16;
    config.work_stealing_enabled = true;
    config.numa_aware = true;
    config.work_steal_batch_size = 8;
    config.actor_memory_pool_size = 4 * 1024 * 1024;  // 4MB
    config.message_memory_pool_size = 8 * 1024 * 1024; // 8MB
    return config;
}

ActorSystemConfig actor_system_reliable_config(void) {
    ActorSystemConfig config = actor_system_default_config();
    config.enable_deadlock_detection = true;
    config.enable_actor_monitoring = true;
    config.enable_performance_tracking = true;
    config.max_messages_per_actor = 50; // More conservative
    return config;
}

// Worker thread function
static void* actor_worker_thread(void* arg) {
    ActorSystem* system = (ActorSystem*)arg;
    
    while (system->is_running && !system->shutdown_requested) {
        pthread_mutex_lock(&system->system_mutex);
        
        // Wait for work if no messages available
        while (system->global_queue_size == 0 && 
               system->is_running && !system->shutdown_requested) {
            pthread_cond_wait(&system->work_available, &system->system_mutex);
        }
        
        if (system->shutdown_requested) {
            pthread_mutex_unlock(&system->system_mutex);
            break;
        }
        
        // Get message from global queue
        Message* message = NULL;
        if (system->global_message_queue) {
            message = system->global_message_queue;
            system->global_message_queue = message->next;
            if (!system->global_message_queue) {
                system->global_queue_tail = NULL;
            }
            system->global_queue_size--;
        }
        
        pthread_mutex_unlock(&system->system_mutex);
        
        if (message) {
            // Find target actor and deliver message
            Actor* target = NULL;
            for (size_t i = 0; i < system->actor_count; i++) {
                if (system->actors[i]->id == message->sender_id) {
                    target = system->actors[i];
                    break;
                }
            }
            
            if (target && target->state == ACTOR_STATE_ACTIVE) {
                pthread_mutex_lock(&target->actor_mutex);
                
                // Add message to actor's queue
                if (!target->message_queue) {
                    target->message_queue = message;
                    target->message_queue_tail = message;
                } else {
                    target->message_queue_tail->next = message;
                    target->message_queue_tail = message;
                }
                target->queue_size++;
                message->next = NULL;
                
                pthread_cond_signal(&target->message_available);
                pthread_mutex_unlock(&target->actor_mutex);
                
                // Process message
                current_actor = target;
                uint64_t start_time = get_monotonic_time_ms();
                
                Result_void_ptr result = target->behavior.handle_message(target, message);
                
                uint64_t processing_time = get_monotonic_time_ms() - start_time;
                target->messages_processed++;
                target->total_processing_time_ns += processing_time * 1000000;
                
                if (result.is_error) {
                    target->last_error = result.error;
                    target->error_count++;
                    
                    if (target->behavior.handle_error) {
                        target->behavior.handle_error(target, result.error);
                    }
                    
                    if (target->restart_on_error) {
                        actor_restart(target);
                    }
                }
                
                current_actor = NULL;
                
                // Clean up message
                if (message->owns_payload && message->payload) {
                    if (message->payload_destructor) {
                        message->payload_destructor(message->payload);
                    } else {
                        free(message->payload);
                    }
                }
                free(message);
            } else {
                // Target actor not found or not active, clean up message
                if (message->owns_payload && message->payload) {
                    if (message->payload_destructor) {
                        message->payload_destructor(message->payload);
                    } else {
                        free(message->payload);
                    }
                }
                free(message);
            }
        }
    }
    
    return NULL;
}

// Deadlock detector thread
static void* deadlock_detector_thread(void* arg) {
    ActorSystem* system = (ActorSystem*)arg;
    
    while (system->is_running && !system->shutdown_requested) {
        sleep(1); // Check every second
        
        if (system->deadlock_detection_enabled) {
            DeadlockInfo info = {0};
            if (actor_system_detect_deadlock(system, &info)) {
                // Attempt to resolve deadlock
                actor_system_resolve_deadlock(system, &info);
            }
        }
    }
    
    return NULL;
}

// Actor system management
ActorSystem* actor_system_create(ActorSystemConfig config) {
    ActorSystem* system = calloc(1, sizeof(ActorSystem));
    if (!system) return NULL;
    
    system->config = config;
    system->next_actor_id = 1;
    system->is_running = false;
    system->shutdown_requested = false;
    
    // Initialize synchronization primitives
    if (pthread_mutex_init(&system->system_mutex, NULL) != 0) {
        free(system);
        return NULL;
    }
    
    if (pthread_cond_init(&system->work_available, NULL) != 0) {
        pthread_mutex_destroy(&system->system_mutex);
        free(system);
        return NULL;
    }
    
    if (pthread_cond_init(&system->system_shutdown, NULL) != 0) {
        pthread_mutex_destroy(&system->system_mutex);
        pthread_cond_destroy(&system->work_available);
        free(system);
        return NULL;
    }
    
    // Allocate actor array
    system->actor_capacity = config.max_actors;
    system->actors = calloc(system->actor_capacity, sizeof(Actor*));
    if (!system->actors) {
        pthread_mutex_destroy(&system->system_mutex);
        pthread_cond_destroy(&system->work_available);
        pthread_cond_destroy(&system->system_shutdown);
        free(system);
        return NULL;
    }
    
    // Allocate worker threads
    system->worker_count = config.thread_pool_size;
    system->worker_threads = calloc(system->worker_count, sizeof(pthread_t));
    if (!system->worker_threads) {
        free(system->actors);
        pthread_mutex_destroy(&system->system_mutex);
        pthread_cond_destroy(&system->work_available);
        pthread_cond_destroy(&system->system_shutdown);
        free(system);
        return NULL;
    }
    
    // Allocate error array
    system->error_capacity = 100;
    system->system_errors = calloc(system->error_capacity, sizeof(Error*));
    if (!system->system_errors) {
        free(system->worker_threads);
        free(system->actors);
        pthread_mutex_destroy(&system->system_mutex);
        pthread_cond_destroy(&system->work_available);
        pthread_cond_destroy(&system->system_shutdown);
        free(system);
        return NULL;
    }
    
    // Initialize memory pools if enabled
    if (config.use_memory_pools) {
        system->actor_memory_pool = malloc(config.actor_memory_pool_size);
        system->message_memory_pool = malloc(config.message_memory_pool_size);
        
        if (!system->actor_memory_pool || !system->message_memory_pool) {
            free(system->actor_memory_pool);
            free(system->message_memory_pool);
            free(system->system_errors);
            free(system->worker_threads);
            free(system->actors);
            pthread_mutex_destroy(&system->system_mutex);
            pthread_cond_destroy(&system->work_available);
            pthread_cond_destroy(&system->system_shutdown);
            free(system);
            return NULL;
        }
    }
    
    system->deadlock_detection_enabled = config.enable_deadlock_detection;
    
    return system;
}

Result_void_ptr actor_system_start(ActorSystem* system) {
    if (!system) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor system"),
            .hint = strdup("Ensure system is properly initialized"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->is_running) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(NULL);
    }
    
    system->is_running = true;
    system->shutdown_requested = false;
    
    // Create guardian actor (root supervisor)
    ActorBehavior guardian_behavior = {
        .handle_message = NULL, // Guardian handles system messages
        .handle_error = NULL,
        .on_start = NULL,
        .on_stop = NULL,
        .on_child_terminated = NULL,
        .state_size = 0,
        .init_state = NULL,
        .cleanup_state = NULL
    };
    
    system->guardian_actor = actor_spawn(system, guardian_behavior, "guardian");
    if (!system->guardian_actor) {
        system->is_running = false;
        pthread_mutex_unlock(&system->system_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create guardian actor"),
            .hint = strdup("Check system resources and configuration"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    system->guardian_actor->is_system_actor = true;
    
    // Start worker threads
    for (size_t i = 0; i < system->worker_count; i++) {
        if (pthread_create(&system->worker_threads[i], NULL, actor_worker_thread, system) != 0) {
            // Clean up already started threads
            system->shutdown_requested = true;
            pthread_cond_broadcast(&system->work_available);
            
            for (size_t j = 0; j < i; j++) {
                pthread_join(system->worker_threads[j], NULL);
            }
            
            system->is_running = false;
            pthread_mutex_unlock(&system->system_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INTERNAL,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to start worker threads"),
                .hint = strdup("Check system thread limits and resources"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    // Start deadlock detector if enabled
    if (system->deadlock_detection_enabled) {
        if (pthread_create(&system->deadlock_detector_thread, NULL, deadlock_detector_thread, system) != 0) {
            // Non-fatal, continue without deadlock detection
            system->deadlock_detection_enabled = false;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr actor_system_shutdown(ActorSystem* system, uint64_t timeout_ms) {
    if (!system) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor system"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (!system->is_running) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(NULL);
    }
    
    system->shutdown_requested = true;
    
    // Stop all actors
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i]) {
            actor_stop(system->actors[i]);
        }
    }
    
    // Wake up all worker threads
    pthread_cond_broadcast(&system->work_available);
    
    pthread_mutex_unlock(&system->system_mutex);
    
    // Wait for worker threads to finish
    for (size_t i = 0; i < system->worker_count; i++) {
        pthread_join(system->worker_threads[i], NULL);
    }
    
    // Stop deadlock detector
    if (system->deadlock_detection_enabled) {
        pthread_join(system->deadlock_detector_thread, NULL);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    system->is_running = false;
    pthread_cond_broadcast(&system->system_shutdown);
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK_PTR(NULL);
}

void actor_system_destroy(ActorSystem* system) {
    if (!system) return;
    
    // Ensure system is shut down
    if (system->is_running) {
        actor_system_shutdown(system, 5000); // 5 second timeout
    }
    
    // Clean up actors
    if (system->actors) {
        for (size_t i = 0; i < system->actor_count; i++) {
            if (system->actors[i]) {
                // Clean up actor state
                if (system->actors[i]->behavior.cleanup_state && system->actors[i]->actor_state) {
                    system->actors[i]->behavior.cleanup_state(system->actors[i]->actor_state);
                }
                
                // Clean up message queue
                Message* msg = system->actors[i]->message_queue;
                while (msg) {
                    Message* next = msg->next;
                    if (msg->owns_payload && msg->payload && msg->payload_destructor) {
                        msg->payload_destructor(msg->payload);
                    }
                    free(msg);
                    msg = next;
                }
                
                // Clean up children array
                free(system->actors[i]->children);
                
                // Destroy synchronization primitives
                pthread_mutex_destroy(&system->actors[i]->actor_mutex);
                pthread_cond_destroy(&system->actors[i]->message_available);
                
                free(system->actors[i]);
            }
        }
        free(system->actors);
    }
    
    // Clean up errors
    if (system->system_errors) {
        for (size_t i = 0; i < system->error_count; i++) {
            if (system->system_errors[i]) {
                free((void*)system->system_errors[i]->message);
                free((void*)system->system_errors[i]->hint);
                free(system->system_errors[i]);
            }
        }
        free(system->system_errors);
    }
    
    // Clean up memory pools
    free(system->actor_memory_pool);
    free(system->message_memory_pool);
    
    // Clean up worker threads array
    free(system->worker_threads);
    
    // Clean up global message queue
    Message* msg = system->global_message_queue;
    while (msg) {
        Message* next = msg->next;
        if (msg->owns_payload && msg->payload && msg->payload_destructor) {
            msg->payload_destructor(msg->payload);
        }
        free(msg);
        msg = next;
    }
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&system->system_mutex);
    pthread_cond_destroy(&system->work_available);
    pthread_cond_destroy(&system->system_shutdown);
    
    free(system);
}

// Actor lifecycle
Actor* actor_spawn(ActorSystem* system, ActorBehavior behavior, const char* name) {
    if (!system || !name) return NULL;
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->actor_count >= system->actor_capacity) {
        pthread_mutex_unlock(&system->system_mutex);
        return NULL;
    }
    
    Actor* actor = calloc(1, sizeof(Actor));
    if (!actor) {
        pthread_mutex_unlock(&system->system_mutex);
        return NULL;
    }
    
    // Initialize actor
    actor->id = generate_unique_id();
    strncpy(actor->name, name, sizeof(actor->name) - 1);
    actor->name[sizeof(actor->name) - 1] = '\0';
    actor->state = ACTOR_STATE_INITIALIZING;
    actor->system = system;
    actor->behavior = behavior;
    actor->restart_on_error = true;
    atomic_init(&actor->ref_count, 1);
    
    // Initialize synchronization
    if (pthread_mutex_init(&actor->actor_mutex, NULL) != 0) {
        free(actor);
        pthread_mutex_unlock(&system->system_mutex);
        return NULL;
    }
    
    if (pthread_cond_init(&actor->message_available, NULL) != 0) {
        pthread_mutex_destroy(&actor->actor_mutex);
        free(actor);
        pthread_mutex_unlock(&system->system_mutex);
        return NULL;
    }
    
    // Initialize children array
    actor->child_capacity = 10;
    actor->children = calloc(actor->child_capacity, sizeof(Actor*));
    if (!actor->children) {
        pthread_mutex_destroy(&actor->actor_mutex);
        pthread_cond_destroy(&actor->message_available);
        free(actor);
        pthread_mutex_unlock(&system->system_mutex);
        return NULL;
    }
    
    // Initialize actor state if needed
    if (behavior.state_size > 0) {
        actor->actor_state = calloc(1, behavior.state_size);
        if (!actor->actor_state) {
            free(actor->children);
            pthread_mutex_destroy(&actor->actor_mutex);
            pthread_cond_destroy(&actor->message_available);
            free(actor);
            pthread_mutex_unlock(&system->system_mutex);
            return NULL;
        }
        
        if (behavior.init_state) {
            behavior.init_state(actor->actor_state);
        }
    }
    
    actor->max_queue_size = system->config.max_messages_per_actor;
    
    // Add to system
    system->actors[system->actor_count++] = actor;
    system->total_actors_created++;
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return actor;
}

Actor* actor_spawn_child(Actor* parent, ActorBehavior behavior, const char* name) {
    if (!parent || !parent->system) return NULL;
    
    Actor* child = actor_spawn(parent->system, behavior, name);
    if (!child) return NULL;
    
    pthread_mutex_lock(&parent->actor_mutex);
    
    // Add to parent's children
    if (parent->child_count >= parent->child_capacity) {
        // Resize children array
        size_t new_capacity = parent->child_capacity * 2;
        Actor** new_children = realloc(parent->children, new_capacity * sizeof(Actor*));
        if (!new_children) {
            pthread_mutex_unlock(&parent->actor_mutex);
            actor_terminate(child, NULL);
            return NULL;
        }
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    
    pthread_mutex_unlock(&parent->actor_mutex);
    
    return child;
}

Result_void_ptr actor_start(Actor* actor) {
    if (!actor) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    if (actor->state != ACTOR_STATE_INITIALIZING) {
        pthread_mutex_unlock(&actor->actor_mutex);
        return OK_PTR(NULL);
    }
    
    actor->state = ACTOR_STATE_ACTIVE;
    
    // Call on_start callback
    if (actor->behavior.on_start) {
        Result_void_ptr result = actor->behavior.on_start(actor);
        if (result.is_error) {
            actor->state = ACTOR_STATE_ERROR;
            actor->last_error = result.error;
            pthread_mutex_unlock(&actor->actor_mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&actor->actor_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr actor_stop(Actor* actor) {
    if (!actor) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    if (actor->state == ACTOR_STATE_TERMINATED || actor->state == ACTOR_STATE_TERMINATING) {
        pthread_mutex_unlock(&actor->actor_mutex);
        return OK_PTR(NULL);
    }
    
    actor->state = ACTOR_STATE_TERMINATING;
    
    // Call on_stop callback
    if (actor->behavior.on_stop) {
        Result_void_ptr result = actor->behavior.on_stop(actor);
        if (result.is_error) {
            // Log error but continue shutdown
            actor->last_error = result.error;
        }
    }
    
    actor->state = ACTOR_STATE_TERMINATED;
    
    pthread_mutex_unlock(&actor->actor_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr actor_terminate(Actor* actor, Error* reason) {
    if (!actor) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Stop all children first
    pthread_mutex_lock(&actor->actor_mutex);
    for (size_t i = 0; i < actor->child_count; i++) {
        if (actor->children[i]) {
            actor_terminate(actor->children[i], reason);
        }
    }
    pthread_mutex_unlock(&actor->actor_mutex);
    
    // Stop this actor
    Result_void_ptr result = actor_stop(actor);
    
    if (reason) {
        actor->last_error = reason;
    }
    
    // Notify parent of termination
    if (actor->parent && actor->parent->behavior.on_child_terminated) {
        actor->parent->behavior.on_child_terminated(actor->parent, actor, reason);
    }
    
    // Update system metrics
    if (actor->system) {
        pthread_mutex_lock(&actor->system->system_mutex);
        actor->system->total_actors_terminated++;
        pthread_mutex_unlock(&actor->system->system_mutex);
    }
    
    return result;
}

// Message creation and sending
Message* message_create(const char* name, void* payload, size_t payload_size) {
    Message* message = calloc(1, sizeof(Message));
    if (!message) return NULL;
    
    message->type = MSG_TYPE_USER;
    message->message_id = generate_unique_id();
    message->timestamp = get_monotonic_time_ms();
    message->message_name = name ? strdup(name) : NULL;
    message->payload = payload;
    message->payload_size = payload_size;
    message->owns_payload = true;
    message->expects_response = false;
    message->priority = 0;
    message->timeout_ms = 30000; // 30 second default timeout
    
    return message;
}

Message* message_create_with_timeout(const char* name, void* payload, 
                                   size_t payload_size, uint64_t timeout_ms) {
    Message* message = message_create(name, payload, payload_size);
    if (message) {
        message->timeout_ms = timeout_ms;
    }
    return message;
}

void message_destroy(Message* message) {
    if (!message) return;
    
    if (message->owns_payload && message->payload) {
        if (message->payload_destructor) {
            message->payload_destructor(message->payload);
        } else {
            free(message->payload);
        }
    }
    
    free((void*)message->message_name);
    free(message);
}

Future* actor_send(Actor* target, const char* message_name, void* payload, size_t payload_size) {
    return actor_send_with_timeout(target, message_name, payload, payload_size, 30000);
}

Future* actor_send_with_timeout(Actor* target, const char* message_name, 
                               void* payload, size_t payload_size, uint64_t timeout_ms) {
    if (!target || !target->system) return NULL;
    
    Message* message = message_create_with_timeout(message_name, payload, payload_size, timeout_ms);
    if (!message) return NULL;
    
    // Create future for response
    Future* future = calloc(1, sizeof(Future));
    if (!future) {
        message_destroy(message);
        return NULL;
    }
    
    future->id = generate_unique_id();
    future->creation_time = get_monotonic_time_ms();
    future->timeout_ms = timeout_ms;
    future->waiting_actor = current_actor;
    
    if (pthread_mutex_init(&future->future_mutex, NULL) != 0) {
        free(future);
        message_destroy(message);
        return NULL;
    }
    
    if (pthread_cond_init(&future->completed, NULL) != 0) {
        pthread_mutex_destroy(&future->future_mutex);
        free(future);
        message_destroy(message);
        return NULL;
    }
    
    message->response_future = future;
    message->expects_response = true;
    message->sender = current_actor;
    message->sender_id = target->id; // Set target for worker to find
    
    // Queue message in system's global queue
    pthread_mutex_lock(&target->system->system_mutex);
    
    if (!target->system->global_message_queue) {
        target->system->global_message_queue = message;
        target->system->global_queue_tail = message;
    } else {
        target->system->global_queue_tail->next = message;
        target->system->global_queue_tail = message;
    }
    target->system->global_queue_size++;
    target->system->total_messages_sent++;
    
    // Wake up a worker
    pthread_cond_signal(&target->system->work_available);
    
    pthread_mutex_unlock(&target->system->system_mutex);
    
    return future;
}

Result_void_ptr actor_send_async(Actor* target, const char* message_name, 
                                void* payload, size_t payload_size) {
    if (!target || !target->system) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid target actor or system"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    Message* message = message_create(message_name, payload, payload_size);
    if (!message) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create message"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    message->sender = current_actor;
    message->sender_id = target->id;
    message->expects_response = false;
    
    // Queue message
    pthread_mutex_lock(&target->system->system_mutex);
    
    if (!target->system->global_message_queue) {
        target->system->global_message_queue = message;
        target->system->global_queue_tail = message;
    } else {
        target->system->global_queue_tail->next = message;
        target->system->global_queue_tail = message;
    }
    target->system->global_queue_size++;
    target->system->total_messages_sent++;
    
    pthread_cond_signal(&target->system->work_available);
    pthread_mutex_unlock(&target->system->system_mutex);
    
    return OK_PTR(NULL);
}

// Future operations
Result_void_ptr future_await(Future* future) {
    return future_await_timeout(future, future->timeout_ms);
}

Result_void_ptr future_await_timeout(Future* future, uint64_t timeout_ms) {
    if (!future) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid future"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&future->future_mutex);
    
    if (future->is_completed) {
        pthread_mutex_unlock(&future->future_mutex);
        if (future->is_error) {
            return ERR_PTR(future->error);
        } else {
            return OK_PTR(future->result);
        }
    }
    
    // Wait with timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    
    int result = pthread_cond_timedwait(&future->completed, &future->future_mutex, &deadline);
    
    if (result == ETIMEDOUT) {
        pthread_mutex_unlock(&future->future_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Future timed out"),
            .hint = strdup("Increase timeout or check actor responsiveness"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    bool is_error = future->is_error;
    void* return_result = future->result;
    Error* return_error = future->error;
    
    pthread_mutex_unlock(&future->future_mutex);
    
    if (is_error) {
        return ERR_PTR(return_error);
    } else {
        return OK_PTR(return_result);
    }
}

bool future_is_ready(Future* future) {
    if (!future) return false;
    
    pthread_mutex_lock(&future->future_mutex);
    bool ready = future->is_completed;
    pthread_mutex_unlock(&future->future_mutex);
    
    return ready;
}

void future_destroy(Future* future) {
    if (!future) return;
    
    pthread_mutex_destroy(&future->future_mutex);
    pthread_cond_destroy(&future->completed);
    
    if (future->result_destructor && future->result) {
        future->result_destructor(future->result);
    }
    
    free(future);
}

// System metrics
ActorSystemMetrics actor_system_get_metrics(ActorSystem* system) {
    ActorSystemMetrics metrics = {0};
    
    if (!system) return metrics;
    
    pthread_mutex_lock(&system->system_mutex);
    
    metrics.total_actors = system->total_actors_created;
    metrics.active_actors = 0;
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i]->state == ACTOR_STATE_ACTIVE) {
            metrics.active_actors++;
        }
    }
    
    metrics.total_messages_sent = system->total_messages_sent;
    metrics.total_messages_processed = system->total_messages_processed;
    
    // Calculate average message latency
    uint64_t total_processing_time = 0;
    uint64_t total_messages = 0;
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i]) {
            total_processing_time += system->actors[i]->total_processing_time_ns;
            total_messages += system->actors[i]->messages_processed;
        }
    }
    
    if (total_messages > 0) {
        metrics.average_message_latency_ns = total_processing_time / total_messages;
    }
    
    metrics.peak_message_queue_size = system->global_queue_size;
    metrics.memory_usage_bytes = system->pool_allocations;
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return metrics;
}

void actor_system_reset_metrics(ActorSystem* system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->system_mutex);
    
    system->total_messages_sent = 0;
    system->total_messages_processed = 0;
    system->total_actors_created = 0;
    system->total_actors_terminated = 0;
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i]) {
            system->actors[i]->messages_processed = 0;
            system->actors[i]->total_processing_time_ns = 0;
            system->actors[i]->error_count = 0;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
}

// Basic deadlock detection (simplified)
bool actor_system_detect_deadlock(ActorSystem* system, DeadlockInfo* info) {
    if (!system || !info) return false;
    
    // This is a simplified deadlock detection
    // In a full implementation, this would use graph algorithms
    // to detect circular dependencies in message passing
    
    pthread_mutex_lock(&system->system_mutex);
    
    // Check for actors waiting too long
    uint64_t current_time = get_monotonic_time_ms();
    
    for (size_t i = 0; i < system->actor_count; i++) {
        Actor* actor = system->actors[i];
        if (!actor) continue;
        
        if (actor->state == ACTOR_STATE_ACTIVE && actor->queue_size > 0) {
            // Check if actor has been processing for too long without progress
            // This is a heuristic - real deadlock detection would be more sophisticated
            if (current_time - actor->total_processing_time_ns > 10000) { // 10 seconds
                info->actors_involved = malloc(sizeof(Actor*));
                info->actors_involved[0] = actor;
                info->actor_count = 1;
                info->detection_time = current_time;
                info->deadlock_description = "Potential deadlock detected: actor unresponsive";
                
                pthread_mutex_unlock(&system->system_mutex);
                return true;
            }
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    return false;
}

Result_void_ptr actor_system_resolve_deadlock(ActorSystem* system, DeadlockInfo* info) {
    if (!system || !info) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid system or deadlock info"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Simple deadlock resolution: restart affected actors
    for (size_t i = 0; i < info->actor_count; i++) {
        if (info->actors_involved[i]) {
            actor_restart(info->actors_involved[i]);
        }
    }
    
    return OK_PTR(NULL);
}

// Actor discovery
Actor* actor_find_by_name(ActorSystem* system, const char* name) {
    if (!system || !name) return NULL;
    
    pthread_mutex_lock(&system->system_mutex);
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && strcmp(system->actors[i]->name, name) == 0) {
            Actor* found = system->actors[i];
            pthread_mutex_unlock(&system->system_mutex);
            return found;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    return NULL;
}

Actor* actor_find_by_id(ActorSystem* system, uint64_t id) {
    if (!system) return NULL;
    
    pthread_mutex_lock(&system->system_mutex);
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i]->id == id) {
            Actor* found = system->actors[i];
            pthread_mutex_unlock(&system->system_mutex);
            return found;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    return NULL;
}

Result_void_ptr actor_restart(Actor* actor) {
    if (!actor) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid actor"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Stop the actor
    Result_void_ptr stop_result = actor_stop(actor);
    if (stop_result.is_error) {
        return stop_result;
    }
    
    // Reset actor state
    pthread_mutex_lock(&actor->actor_mutex);
    
    // Clear message queue
    Message* msg = actor->message_queue;
    while (msg) {
        Message* next = msg->next;
        message_destroy(msg);
        msg = next;
    }
    actor->message_queue = NULL;
    actor->message_queue_tail = NULL;
    actor->queue_size = 0;
    
    // Reset error state
    actor->last_error = NULL;
    actor->error_count = 0;
    
    // Reset state
    actor->state = ACTOR_STATE_INITIALIZING;
    
    pthread_mutex_unlock(&actor->actor_mutex);
    
    // Start the actor again
    return actor_start(actor);
}