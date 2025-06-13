05-CONCURRENCY-MODEL.md
markdown# Goo Concurrency Model

## Overview

Goo provides a unified concurrency model that combines Go's simplicity with Rust's safety guarantees and Erlang's fault tolerance. The model is built on lightweight goroutines, type-safe channels, and actor-based programming.

## Core Primitives

### Goroutines

#### Basic Usage
```goo
// Spawn a goroutine
go print("Hello from goroutine")

// With closure
let x = 42
go func() {
    print(f"x = {x}")
}()

// Named function
func worker(id: int) {
    print(f"Worker {id} starting")
}
go worker(1)
Structured Concurrency
goo// Scoped goroutines
func process_data(items: []Item) ![]Result {
    mut results = vec<Result>[]
    
    // All goroutines must complete before returning
    parallel {
        for item in items {
            go func(it: Item) {
                results.push(process(it))
            }(item)
        }
    }
    
    return results
}

// With cancellation
func fetch_with_timeout(urls: []string) ![]Response {
    let ctx = Context.with_timeout(5s)
    
    return parallel(ctx) {
        for url in urls {
            go fetch(ctx, url)
        }
    }
}
Goroutine Lifecycle
goo// Spawn with handle
let handle = spawn {
    long_running_task()
}

// Wait for completion
handle.join()

// Check if done
if handle.is_finished() {
    let result = handle.result()
}

// Cancel goroutine
handle.cancel()
Channels
Channel Types
goo// Simple channel
let ch = channel<int>()

// Buffered channel
let ch = channel<string>(100)

// Directional channels
let send_only: chan<- int = ch
let recv_only: <-chan int = ch

// Advanced patterns
let pub_ch = channel pub<Event>()      // Publisher
let sub_ch = channel sub<Event>()      // Subscriber
let req_ch = channel req<Request>()    // Request
let rep_ch = channel rep<Response>()   // Reply
let push_ch = channel push<Task>()     // Work distribution
let pull_ch = channel pull<Task>()     // Work collection
Channel Operations
goo// Send
ch <- 42

// Receive
let value = <-ch

// Non-blocking operations
select {
    case ch <- value:
        print("Sent")
    default:
        print("Channel full")
}

// Multi-way select with timeout
select {
    case msg = <-ch1:
        handle_ch1(msg)
    case msg = <-ch2:
        handle_ch2(msg)
    case <-timeout(1s):
        print("Timeout")
}

// Channel closing
close(ch)

// Receive with closed check
if let Some(value) = <-ch {
    process(value)
} else {
    print("Channel closed")
}
Channel Patterns
Fan-In/Fan-Out
goo// Fan-out: distribute work
func fan_out(input: <-chan Task, workers: int) []<-chan Result {
    mut outputs = vec[]
    
    for i in 0..workers {
        out := channel<Result>()
        outputs.push(out)
        
        go func(in: <-chan Task, out: chan<- Result) {
            for task in in {
                out <- process(task)
            }
            close(out)
        }(input, out)
    }
    
    return outputs
}

// Fan-in: merge results
func fan_in(inputs: []<-chan Result) <-chan Result {
    let out = channel<Result>()
    let wg = WaitGroup.new()
    
    for input in inputs {
        wg.add(1)
        go func(ch: <-chan Result) {
            for result in ch {
                out <- result
            }
            wg.done()
        }(input)
    }
    
    go func() {
        wg.wait()
        close(out)
    }()
    
    return out
}
Actors
Actor Definition
gooactor Counter {
    mut count: int = 0
    mut subscribers: vec<chan<int>> = vec[]
    
    // Message handlers
    handle Increment(amount: int = 1) {
        self.count += amount
        self.notify_subscribers()
    }
    
    handle Decrement(amount: int = 1) {
        self.count -= amount
        self.notify_subscribers()
    }
    
    handle GetCount() -> int {
        return self.count
    }
    
    handle Subscribe(ch: chan<int>) {
        self.subscribers.push(ch)
        ch <- self.count  // Send current value
    }
    
    // Private method
    func notify_subscribers(self) {
        for ch in self.subscribers {
            select {
                case ch <- self.count:
                    // Sent successfully
                default:
                    // Subscriber not ready, skip
            }
        }
    }
}
Actor Usage
goo// Create actor
let counter = Counter.spawn()

// Send messages (fire-and-forget)
counter ! Increment(5)
counter ! Decrement(2)

// Request-reply
let count = counter ? GetCount()
print(f"Count: {count}")

// Async request
let future = counter ?? GetCount()
// Do other work...
let count = future.await()

// Subscribe to updates
let updates = channel<int>(10)
counter ! Subscribe(updates)
Actor Supervision
goo// Supervisor actor
actor Supervisor {
    mut workers: map[int]Worker = map[]
    
    handle StartWorker(id: int) {
        self.workers[id] = Worker.spawn_linked(self)
    }
    
    handle WorkerCrashed(id: int, error: Error) {
        print(f"Worker {id} crashed: {error}")
        
        // Restart strategy
        match self.restart_strategy {
            OneForOne => {
                self.workers[id] = Worker.spawn_linked(self)
            }
            OneForAll => {
                self.restart_all_workers()
            }
            RestForOne => {
                self.restart_workers_after(id)
            }
        }
    }
}

// Supervision tree
supervised {
    strategy: OneForOne,
    max_restarts: 3,
    time_window: 60s
} {
    spawn WebServer()
    spawn Database()
    spawn Cache()
}
Actor Patterns
Request-Reply Pattern
gooactor Service {
    handle Request(data: Data, reply_to: chan<Response>) {
        let result = self.process(data)
        reply_to <- Response { result }
    }
}

// Usage
let reply = channel<Response>(1)
service ! Request(data, reply)
let response = <-reply
State Machine Actor
gooactor Connection {
    state: enum {
        Disconnected,
        Connecting,
        Connected,
        Disconnecting,
    } = Disconnected
    
    handle Connect() {
        match self.state {
            Disconnected => {
                self.state = Connecting
                self.do_connect()
            }
            _ => error("Invalid state for connect")
        }
    }
    
    handle DataReceived(data: []byte) {
        match self.state {
            Connected => self.process_data(data),
            _ => error("Not connected")
        }
    }
}
Parallel Execution
Parallel Loops
goo// Basic parallel for
parallel for i in 0..1000 {
    process(data[i])
}

// With configuration
parallel {
    workers: 8,
    chunk_size: 100,
    schedule: Dynamic
} for item in items {
    expensive_computation(item)
}

// Nested parallelism
parallel for row in matrix {
    parallel for cell in row {
        cell.update()
    }
}

// With reduction
let sum = parallel reduce(0) for x in numbers {
    x * x  // Square each number
} combine |a, b| {
    a + b  // Sum them up
}
Map-Reduce
goo// Parallel map
let results = parallel map(items, |item| {
    transform(item)
})

// Parallel filter
let filtered = parallel filter(items, |item| {
    item.is_valid()
})

// Parallel reduce
let sum = parallel reduce(numbers, 0, |a, b| a + b)

// Combined pipeline
let total = items
    |> parallel map(process)
    |> parallel filter(valid)
    |> parallel reduce(combine)
Task Parallelism
goo// Fork-join parallelism
let (a, b, c) = parallel {
    compute_a(),
    compute_b(), 
    compute_c()
}

// With error handling
let results = try parallel {
    fetch_from_api1(),
    fetch_from_api2(),
    fetch_from_api3()
} else err {
    return default_results()
}

// Task groups
let group = TaskGroup.new()
group.spawn(task1)
group.spawn(task2)
group.spawn(task3)
let results = group.join_all()
Synchronization Primitives
Atomic Operations
goo// Atomic types
let counter = atomic<int>(0)
counter.add(1)
counter.compare_exchange(0, 1)

// Memory ordering
counter.store(42, Release)
let value = counter.load(Acquire)

// Atomic references
let ptr = atomic<*Node>(null)
let old = ptr.swap(new_node)
Mutexes and RwLocks
goo// Mutex
let mutex = Mutex.new(data)
{
    let mut guard = mutex.lock()
    guard.modify()
}  // Automatically unlocked

// Try lock
if let Some(mut guard) = mutex.try_lock() {
    guard.modify()
}

// RwLock
let rwlock = RwLock.new(shared_data)

// Multiple readers
parallel for i in 0..10 {
    let data = rwlock.read()
    process(data)
}

// Exclusive writer
{
    let mut data = rwlock.write()
    data.update()
}
Condition Variables
goolet mutex = Mutex.new(state)
let cond = Condvar.new()

// Wait for condition
{
    let mut state = mutex.lock()
    while !state.ready {
        state = cond.wait(state)
    }
    process(state)
}

// Signal condition
{
    let mut state = mutex.lock()
    state.ready = true
    cond.notify_all()
}

// Wait with timeout
{
    let mut state = mutex.lock()
    if let Some(state) = cond.wait_timeout(state, 1s) {
        process(state)
    } else {
        print("Timeout waiting for condition")
    }
}
Barriers and Latches
goo// Barrier - reusable synchronization point
let barrier = Barrier.new(num_workers)

parallel for i in 0..num_workers {
    setup_worker(i)
    barrier.wait()  // All workers wait here
    start_work(i)
    barrier.wait()  // Synchronize again
    cleanup(i)
}

// Latch - one-time synchronization
let latch = CountDownLatch.new(3)

go func() {
    init_database()
    latch.count_down()
}()

go func() {
    init_cache()
    latch.count_down()
}()

go func() {
    init_network()
    latch.count_down()
}()

latch.wait()  // Wait for all initialization
start_application()
Semaphores
goo// Counting semaphore
let sem = Semaphore.new(3)  // Max 3 concurrent

parallel for task in tasks {
    sem.acquire()
    go func() {
        defer sem.release()
        process_task(task)
    }()
}
Memory Model
Happens-Before Relationships

Program Order: Operations in a single goroutine
Channel Order: Send happens-before receive
Lock Order: Unlock happens-before lock
Once Order: First call happens-before subsequent calls
Spawn Order: Parent happens-before child goroutine

Data Race Freedom
goo// Guaranteed safe by type system
shared data = SharedData.new()

parallel for i in 0..1000 {
    data.increment()  // Synchronized access
}

// Compile error for races
let mut vec = vec[1, 2, 3]
go func() {
    vec.push(4)  // Error: vec is not Send
}

// Safe sharing patterns
let data = Arc.new(Mutex.new(vec[]))
go func(data: Arc<Mutex<vec<int>>>) {
    let mut guard = data.lock()
    guard.push(4)  // OK: synchronized
}(data.clone())
Performance Considerations
Work Stealing Scheduler

M:N threading model
Work stealing for load balancing
CPU affinity support
NUMA awareness

goo// Configure scheduler
Runtime.configure {
    worker_threads: 8,
    stack_size: 2MB,
    cpu_affinity: true,
    numa_aware: true
}
Channel Optimizations

Lock-free for single producer/consumer
Batching for small messages
Zero-copy for large messages
Specialized implementations per pattern

goo// High-performance channel
let ch = channel<Message> {
    capacity: 1000,
    strategy: LockFree,
    batching: true,
    zero_copy_threshold: 4KB
}
Actor Optimizations

Local message passing without serialization
Actor placement strategies
Message batching
Priority queues

goo// Optimized actor configuration
actor HighPerformance {
    @message_buffer(1000)
    @placement(NumaNode(0))
    @priority(High)
    
    handle Process(data: Data) {
        // Processing
    }
}
Best Practices
1. Prefer Channels Over Shared Memory
goo// Good: Communication via channels
let results = channel<Result>()
go worker(input, results)
let result = <-results

// Avoid: Shared mutable state
shared mut data = Data.new()
go func() { data.modify() }  // Race condition risk
2. Use Actors for Stateful Components
goo// Good: Encapsulated state
actor Cache {
    mut data: map[string]Value
    
    handle Get(key: string) -> ?Value {
        return self.data.get(key)
    }
}

// Avoid: Exposed mutable state
struct Cache {
    pub mut data: map[string]Value  // External mutation
}
3. Structure Concurrency
goo// Good: Scoped concurrency
parallel {
    task1(),
    task2(),
    task3()
}  // All complete before continuing

// Avoid: Fire-and-forget
go task1()
go task2()
go task3()
// No guarantee when they complete
4. Handle Errors in Concurrent Code
goo// Good: Error propagation
let results = try parallel {
    risky_op1(),
    risky_op2(),
    risky_op3()
} else err {
    match err {
        ConcurrentError(failed) => {
            print(f"Operations {failed} failed")
            return backup_plan()
        }
    }
}

// Avoid: Ignoring errors
go risky_operation()  // Errors lost
5. Avoid Blocking in Async Context
goo// Good: Non-blocking
select {
    case result = <-ch:
        process(result)
    case <-timeout(100ms):
        return default_value()
}

// Avoid: Blocking indefinitely
let result = <-ch  // Could block forever
Debugging Support
Goroutine Inspection
goo// Get current goroutine info
let info = Goroutine.current().info()
print(f"Running in goroutine {info.id}")

// List all goroutines
for g in Goroutine.all() {
    print(f"Goroutine {g.id}: {g.status}")
}

// Deadlock detection
Runtime.enable_deadlock_detection()
Channel Debugging
goo// Channel statistics
let stats = ch.stats()
print(f"Channel: {stats.pending} pending, {stats.capacity} capacity")

// Trace channel operations
ch.enable_tracing()
Race Detection
goo// Enable race detector
@race_detect
func concurrent_function() {
    // Race detector active
}

// Runtime flag
$ goo run --race myprogram.goo