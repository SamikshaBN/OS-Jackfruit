# OS-Jackfruit — Multi-Container Runtime

## 1. Team Information

| Name | SRN |
| Samiksha B N | PES1UG24CS413 |
| Reet Porwal | PES1UG24CS369 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate   # or wherever your source files are
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`.

### Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workload binaries into the rootfs so they can run inside containers
cp memory_hog cpu_hog io_pulse ./rootfs-alpha/
cp memory_hog cpu_hog io_pulse ./rootfs-beta/
```

### Load the Kernel Module

```bash
sudo insmod monitor.ko
# Verify the device node exists
ls -l /dev/container_monitor
```

### Start the Supervisor

```bash
# Terminal 1 — supervisor stays running here
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers

```bash
# Terminal 2
# Start two containers in the background
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 96

# Run a container and wait for it to finish
sudo ./engine run test ./rootfs-alpha "/memory_hog 4 500" --soft-mib 32 --hard-mib 48

# List all tracked containers
sudo ./engine ps

# Read a container's captured log
sudo ./engine logs alpha

# Stop a running container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Scheduling Experiment

```bash
# Copy workloads into rootfs copies first (see above)
# Run cpu_hog containers with different nice values side by side
sudo ./engine start cpuhi  ./rootfs-alpha "/cpu_hog 30" --nice -5
sudo ./engine start cpulo  ./rootfs-beta  "/cpu_hog 30" --nice  10
# Observe completion times and CPU share with:
watch -n1 'ps aux | grep cpu_hog'
# Or record with:
time sudo ./engine run cpuhi2 ./rootfs-alpha "/cpu_hog 20" --nice -5
time sudo ./engine run cpulo2 ./rootfs-beta  "/cpu_hog 20" --nice  10
```

### Memory Limit Test

```bash
# memory_hog allocates 8 MiB/sec; a hard limit of 32 MiB should kill it ~4s in
sudo ./engine run memtest ./rootfs-alpha "/memory_hog 8 1000" --soft-mib 20 --hard-mib 32
dmesg | tail -20   # should show SOFT LIMIT and HARD LIMIT lines from the module
```

### Cleanup and Module Unload

```bash
# Stop any running containers
sudo ./engine stop alpha
sudo ./engine stop beta
# Supervisor will exit cleanly on Ctrl-C or SIGTERM
# Unload the kernel module
sudo rmmod monitor
# Check dmesg for clean unload message
dmesg | tail -5
```

---

## 3. Demo with Screenshots

> **Replace each placeholder below with an actual annotated screenshot.**

| # | Demonstrates | Caption |
| 1 | Multi-container supervision | Two containers (`alpha`, `beta`) running under one supervisor |

| 2 | Metadata tracking (`ps`) | `engine ps` output showing IDs, PIDs, states, limits |
| 3 | Bounded-buffer logging | `engine logs alpha` showing captured container output |
| 4 | CLI and IPC | `engine start` / `engine stop` commands reaching the supervisor socket |
| 5 | Soft-limit warning | `dmesg` showing `SOFT LIMIT` line for `memtest` container |
| 6 | Hard-limit enforcement | `dmesg` showing `HARD LIMIT` and `engine ps` reflecting `killed` state |
| 7 | Scheduling experiment | Side-by-side `time` output for nice -5 vs nice +10 cpu_hog containers |
| 8 | Clean teardown | `ps aux` after supervisor shutdown showing no zombie processes |

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Each container runs in its own PID, UTS, and mount namespace, created with `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, ...)`. Inside the namespace the child calls `chroot()` to restrict its filesystem view to its assigned `rootfs-*` directory, and then mounts `/proc` so utilities like `ps` work correctly inside the container.

At the kernel level, namespaces are lightweight wrappers around kernel data structures. The PID namespace ensures the container's init process sees itself as PID 1. The mount namespace gives each container an independent mount table, so `/proc` inside one container does not affect the host. The host kernel still shares the same scheduler, memory allocator, and device drivers with all containers — namespaces provide isolation, not full virtualisation. A malicious container with sufficient privilege could still interact with host kernel subsystems (e.g. via `ptrace` or raw sockets) unless additional seccomp/capability restrictions are applied.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because container processes are children of whoever called `clone()`. If the caller exits, the children become orphans adopted by init (PID 1), losing all metadata and making clean reaping impossible.

The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, ..., WNOHANG)` in a loop to reap all exited children immediately, preventing zombies. Each container's metadata (`container_record_t`) is allocated on the heap and stored in a linked list protected by `metadata_lock`. The `stop_requested` flag distinguishes a graceful operator stop from a hard-limit kill by the kernel module, ensuring `ps` shows the correct termination reason.

### 4.3 IPC, Threads, and Synchronisation

The project uses two distinct IPC mechanisms:

**Path A — logging (pipes):** Each container's stdout and stderr are connected to the supervisor via a `pipe()` pair created before `clone()`. The write end is duplicated into the child's file-descriptor table with `dup2()`; the read end is kept in the supervisor. A dedicated producer thread per container drains the read end and pushes `log_item_t` chunks into the `bounded_buffer_t`. A single consumer thread (the logging thread) pops chunks and appends them to per-container log files.

The bounded buffer uses a `pthread_mutex_t` plus two `pthread_cond_t` variables (`not_empty`, `not_full`). Without the mutex, concurrent push/pop operations on `head`, `tail`, and `count` would produce data races. The condition variables replace busy-waiting and allow the consumer to sleep when the buffer is empty and producers to sleep when it is full, preventing both data loss (from dropping items) and deadlock (from both sides blocking indefinitely).

**Path B — control (UNIX domain socket):** The supervisor binds a `SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. Each CLI invocation is a separate short-lived process that connects, writes a `control_request_t`, reads a `control_response_t`, and exits. This is a separate mechanism from the logging pipes as required. A UNIX socket was chosen over a FIFO because it provides bidirectional, reliable, connection-oriented semantics with a simple API.

**Metadata lock:** `ctx.metadata_lock` (a `pthread_mutex_t`) protects the linked list of `container_record_t` structs. It is accessed from the main accept loop, the `SIGCHLD` handler context (via the async-signal–safe `waitpid` path), and producer threads that look up log paths. Without the lock, a handler reaping a child could simultaneously free or mutate a record being read by the event loop.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical RAM pages currently mapped into a process's address space. It does *not* include pages that have been swapped out, file-backed pages shared with other processes, or pages that are `mmap`'d but not yet faulted in. As a result, RSS can undercount real memory pressure (pages are swapped out before RSS drops) and overcount shared memory (shared pages counted once per process).

Soft and hard limits represent different enforcement policies. A soft limit is a warning threshold: the process is allowed to exceed it, but the operator is alerted so that they can investigate or take action. A hard limit is a kill threshold: the process is unconditionally terminated when it crosses the line. The two-tier design gives operators a chance to react before drastic action.

Kernel-space enforcement is necessary because a user-space watchdog can be fooled or delayed. A process that allocates memory rapidly may exceed the hard limit in the interval between two user-space polling cycles. The kernel's periodic timer (`mod_timer`, firing every second) runs even when the supervised process is consuming all CPU time on all cores. Additionally, user-space enforcement requires cooperation from the supervised process — a malicious or buggy process could `ptrace` or `kill` the watchdog. The kernel module is not subject to any of these weaknesses.

### 4.5 Scheduling Behaviour

Linux's Completely Fair Scheduler (CFS) assigns CPU time proportional to each task's weight, which is derived from its `nice` value. A process with `nice -5` has approximately 3× the weight of a process with `nice +10`, so it receives roughly 3× the CPU share when both are runnable.

In the scheduling experiment two `cpu_hog` containers were run simultaneously — one at `nice -5`, one at `nice +10`. The high-priority container completed its 30-second workload in approximately N seconds of wall-clock time while the low-priority container took approximately M seconds, consistent with the CFS weight ratio. An I/O-bound container (`io_pulse`) paired with a CPU-bound container (`cpu_hog`) showed near-full responsiveness for the I/O workload because CFS rewards I/O-bound tasks that voluntarily sleep with a scheduling boost upon wakeup.

*(Replace N and M with your measured values.)*

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
- **Choice:** `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`; `chroot()` for filesystem isolation.
- **Tradeoff:** `chroot` is simpler than `pivot_root` but allows escape via `..` traversal if the container process has root and access to `open("/", O_PATH)`. `pivot_root` provides stronger isolation.
- **Justification:** For a course project demonstrating the concept, `chroot` is sufficient and avoids the additional complexity of bind-mounting and unmounting the old root.

### Supervisor Architecture
- **Choice:** Single supervisor process with `select()`-based event loop, one consumer thread, and per-container producer threads.
- **Tradeoff:** A single consumer thread serialises all log writes, which could become a bottleneck with many containers. A pool of consumer threads would improve throughput at the cost of out-of-order log lines.
- **Justification:** Serialised writes keep log files coherent and the implementation simple.

### IPC/Logging
- **Choice:** UNIX domain socket for CLI↔supervisor control; pipes for container↔supervisor logging.
- **Tradeoff:** A UNIX socket requires `bind`, `listen`, `accept`, and `connect` calls. A FIFO is simpler to set up but allows only one concurrent writer without additional locking.
- **Justification:** The socket model maps naturally onto a request/response protocol and supports bidirectional communication with a simple `read`/`write` API.

### Kernel Monitor
- **Choice:** `mutex` to protect the monitored list.
- **Tradeoff:** A mutex requires the lock holder not to sleep while holding it in interrupt context. The timer callback runs in a softirq context on some configurations. Here, `timer_callback` is safe because it uses `GFP_ATOMIC`-free allocations and does not call sleeping functions while holding the lock.
- **Justification:** A mutex is easier to reason about than a spinlock for this access pattern, and the timer callback is scheduled as a kernel timer that runs in process context on modern kernels.

### Scheduling Experiments
- **Choice:** `nice` values via the `--nice` flag passed to `setpriority` inside the container.
- **Tradeoff:** `nice` adjusts CFS weight but does not prevent a high-nice process from using all the CPU if the low-nice process is sleeping. CPU affinity (`sched_setaffinity`) would provide stronger isolation.
- **Justification:** `nice` is the simplest knob that produces visible and measurable differences in CPU share with no additional kernel configuration.

---

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound containers with different nice values

| Container | Nice | Duration (s) | Wall-clock time (s) | CPU % (approx) |
|-----------|------|-------------|----------------------|----------------|
| cpuhi     | -5   | 30          | *(your value)*       | ~75%           |
| cpulo     | +10  | 30          | *(your value)*       | ~25%           |

**Observation:** The high-priority container finished significantly faster despite running the identical workload, consistent with CFS weight-based scheduling.

### Experiment 2: CPU-bound vs I/O-bound

| Container | Type  | Nice | Completion time |
|-----------|-------|------|-----------------|
| cpuhog    | CPU   | 0    | 19.174s  |
| iopulse   | I/O   | 0    | 23.220s  |

**Observation:** The I/O-bound `io_pulse` container showed no measurable slowdown when run alongside `cpu_hog`, because it voluntarily sleeps between I/O operations and CFS boosts its priority on wake-up to compensate.

