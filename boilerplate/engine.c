/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation covering Tasks 1-3 and 5-6:
 *   - Multi-container runtime with parent supervisor (Task 1)
 *   - Supervisor CLI and signal handling via UNIX domain socket (Task 2)
 *   - Bounded-buffer logging pipeline with producer/consumer threads (Task 3)
 *   - Resource cleanup (Task 6)
 *   - Kernel monitor integration via ioctl (Task 4 user-space side)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  512
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  32
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define MAX_CONTAINERS       64
#define PIPE_READ            0
#define PIPE_WRITE           1

/* ------------------------------------------------------------------ */
/* Enumerations                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data Structures                                                      */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;   /* set before sending SIGTERM/SIGKILL */
    char               log_path[PATH_MAX];
    int                log_read_fd;      /* supervisor-side pipe end for stdout */
    pthread_t          producer_thread;  /* reads pipe, pushes to log buffer   */
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
    int            wait_for_exit;   /* 1 for CMD_RUN */
} control_request_t;

typedef struct {
    int  status;
    int  exit_code;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

/* Passed into clone() child */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;  /* child writes stdout+stderr here */
} child_config_t;

/* Passed to per-container producer thread */
typedef struct {
    int             read_fd;
    char            container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_arg_t;

typedef struct {
    int              server_fd;
    int              monitor_fd;
    volatile int     should_stop;
    pthread_t        logger_thread;   /* single consumer */
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context — needed by signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Usage / Helpers                                                      */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                       */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push — producer inserts one item.
 * Blocks when full; returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer removes one item.
 * Returns  1  if item was retrieved,
 *          0  if shutting down AND buffer is empty,
 *         -1  if shutting down but there are still items (caller should
 *             keep draining).
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return 0;   /* done, nothing left */
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Logging Consumer Thread                                              */
/* ------------------------------------------------------------------ */

/*
 * logging_thread — single consumer.
 * Routes each log chunk to the correct per-container log file.
 * Drains remaining entries before exiting even when shutdown begins.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    while ((rc = bounded_buffer_pop(&ctx->log_buffer, &item)) != 0) {
        char log_path[PATH_MAX];
        int  fd;

        /* Build path: logs/<container_id>.log */
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, item.container_id);

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0, total = (ssize_t)item.length;
            while (written < total) {
                ssize_t n = write(fd, item.data + written,
                                  (size_t)(total - written));
                if (n < 0) break;
                written += n;
            }
            close(fd);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-Container Producer Thread                                        */
/* ------------------------------------------------------------------ */

/*
 * producer_thread_fn — one per container.
 * Reads from the container's pipe and pushes chunks into the bounded buffer.
 */
static void *producer_thread_fn(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    log_item_t      item;
    ssize_t         n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id,
            sizeof(item.container_id) - 1);

    while ((n = read(parg->read_fd, item.data, LOG_CHUNK_SIZE - 1)) > 0) {
        item.length = (size_t)n;
        item.data[n] = '\0';
        /* Push; ignore return — if shutting down we just stop */
        bounded_buffer_push(parg->log_buffer, &item);
        memset(item.data, 0, (size_t)n);
    }

    close(parg->read_fd);
    free(parg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Kernel Monitor Integration                                           */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container Child Entrypoint (runs inside clone'd namespace)           */
/* ------------------------------------------------------------------ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        _exit(1);
    }
    close(cfg->log_write_fd);

    /* Set UTS hostname to container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        _exit(1);
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        _exit(1);
    }

    /* Mount /proc so tools like `ps` work inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal — warn but continue */
        fprintf(stderr, "Warning: failed to mount /proc: %s\n", strerror(errno));
    }

    /* Apply nice value for scheduling experiments */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command */
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    /* If that failed, try the command directly */
    execl(cfg->command, cfg->command, (char *)NULL);
    perror("exec");
    _exit(127);
}

/* ------------------------------------------------------------------ */
/* Metadata Helpers                                                     */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (strcmp(c->id, id) == 0)
            return c;
    return NULL;
}

static void free_container_list(container_record_t *head)
{
    while (head) {
        container_record_t *next = head->next;
        free(head);
        head = next;
    }
}

/* ------------------------------------------------------------------ */
/* Launch a Container                                                   */
/* ------------------------------------------------------------------ */

static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp)
{
    container_record_t *rec;
    child_config_t     *cfg;
    char               *stack, *stack_top;
    pid_t               child_pid;
    int                 pipefd[2];
    producer_arg_t     *parg;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: container '%s' already exists", req->container_id);
        resp->status = -1;
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Prepare the log directory */
    mkdir(LOG_DIR, 0755);

    /* Create stdout/stderr pipe for this container */
    if (pipe(pipefd) != 0) {
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: pipe() failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    /* Build child config */
    cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        close(pipefd[PIPE_READ]);
        close(pipefd[PIPE_WRITE]);
        snprintf(resp->message, sizeof(resp->message), "ERROR: malloc failed");
        resp->status = -1;
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id,      req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs,  req->rootfs,       sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command,      sizeof(cfg->command) - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[PIPE_WRITE];

    /* Allocate clone stack */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[PIPE_READ]);
        close(pipefd[PIPE_WRITE]);
        snprintf(resp->message, sizeof(resp->message), "ERROR: malloc stack");
        resp->status = -1;
        return -1;
    }
    stack_top = stack + STACK_SIZE;

    /* Launch the container with new PID, UTS, and mount namespaces */
    child_pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    free(stack);
    close(pipefd[PIPE_WRITE]);  /* supervisor doesn't write to child */

    if (child_pid < 0) {
        free(cfg);
        close(pipefd[PIPE_READ]);
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: clone() failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }
    free(cfg);

    /* Register with the kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, child_pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Allocate and register metadata */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        kill(child_pid, SIGKILL);
        close(pipefd[PIPE_READ]);
        snprintf(resp->message, sizeof(resp->message), "ERROR: calloc failed");
        resp->status = -1;
        return -1;
    }
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid          = child_pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->log_read_fd       = pipefd[PIPE_READ];
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Start per-container producer thread */
    parg = malloc(sizeof(*parg));
    if (parg) {
        parg->read_fd    = pipefd[PIPE_READ];
        parg->log_buffer = &ctx->log_buffer;
        strncpy(parg->container_id, req->container_id,
                sizeof(parg->container_id) - 1);
        if (pthread_create(&rec->producer_thread, NULL,
                           producer_thread_fn, parg) != 0) {
            free(parg);
            rec->log_read_fd = -1;
        }
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    snprintf(resp->message, sizeof(resp->message),
             "OK: container '%s' started, pid=%d", req->container_id, child_pid);
    resp->status = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal Handlers                                                      */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reap all children that have exited */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c;
        for (c = g_ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;
            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->state     = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                if (WTERMSIG(status) == SIGKILL && !c->stop_requested)
                    c->state = CONTAINER_KILLED;   /* hard-limit kill */
                else
                    c->state = CONTAINER_STOPPED;
            }
            /* Unregister from kernel monitor */
            if (g_ctx->monitor_fd >= 0)
                unregister_from_monitor(g_ctx->monitor_fd,
                                        c->id, c->host_pid);
            break;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Supervisor Event Loop — handles one client request                  */
/* ------------------------------------------------------------------ */

static void handle_request(supervisor_ctx_t *ctx,
                            int client_fd,
                            const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {

    case CMD_START:
    case CMD_RUN:
        launch_container(ctx, req, &resp);
        /* For CMD_RUN: client will wait; supervisor sends back pid */
        if (req->wait_for_exit && resp.status == 0) {
            /* Find the container we just launched */
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c = find_container(ctx, req->container_id);
            if (c) resp.exit_code = c->host_pid;  /* reuse field for pid */
            pthread_mutex_unlock(&ctx->metadata_lock);
        }
        break;

    case CMD_PS: {
        char buf[CONTROL_MESSAGE_LEN * MAX_CONTAINERS];
        int  offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%-16s %-8s %-10s %-10s %-12s %-10s %s\n",
                           "ID", "PID", "STATE",
                           "SOFT(MiB)", "HARD(MiB)", "EXIT", "LOG");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && offset < (int)sizeof(buf) - 256) {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "%-16s %-8d %-10s %-10lu %-12lu %-10d %s\n",
                               c->id, c->host_pid,
                               state_to_string(c->state),
                               c->soft_limit_bytes >> 20,
                               c->hard_limit_bytes >> 20,
                               c->exit_code,
                               c->log_path);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        strncpy(resp.message, buf, sizeof(resp.message) - 1);
        resp.status = 0;
        break;
    }

    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        char log_path[PATH_MAX] = {0};
        if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) {
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: no container '%s'", req->container_id);
            resp.status = -1;
            break;
        }
        /* Send log path so client can read it */
        snprintf(resp.message, sizeof(resp.message),
                 "LOG_PATH:%s", log_path);
        resp.status = 0;
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req->container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: no container '%s'", req->container_id);
            resp.status = -1;
            break;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);
        /* Give it 3 seconds, then SIGKILL */
        int waited = 0;
        while (waited < 3) {
            if (waitpid(pid, NULL, WNOHANG) > 0) break;
            sleep(1);
            waited++;
        }
        kill(pid, SIGKILL);

        snprintf(resp.message, sizeof(resp.message),
                 "OK: sent SIGTERM+SIGKILL to '%s' (pid=%d)",
                 req->container_id, pid);
        resp.status = 0;
        break;
    }

    default:
        snprintf(resp.message, sizeof(resp.message), "ERROR: unknown command");
        resp.status = -1;
        break;
    }

    /* Send response */
    write(client_fd, &resp, sizeof(resp));
}

/* ------------------------------------------------------------------ */
/* Supervisor Main                                                      */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) Open kernel monitor device (optional — continue without it) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: could not open /dev/container_monitor "
                "(kernel module not loaded?): %s\n", strerror(errno));

    /* 2) Create the control UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); goto cleanup;
    }

    /* 3) Install signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* 4) Start the consumer (logging) thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger"); goto cleanup;
    }

    fprintf(stderr, "Supervisor started. base-rootfs=%s  socket=%s\n",
            rootfs, CONTROL_PATH);

    /* 5) Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv = {1, 0};
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;   /* timeout — recheck should_stop */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        control_request_t req;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n == (ssize_t)sizeof(req)) {
            handle_request(&ctx, client_fd, &req);

            /* For CMD_RUN: block until the named container exits */
            if (req.kind == CMD_RUN) {
                container_record_t *c;
                while (1) {
                    pthread_mutex_lock(&ctx.metadata_lock);
                    c = find_container(&ctx, req.container_id);
                    int done = c && (c->state == CONTAINER_EXITED ||
                                     c->state == CONTAINER_STOPPED ||
                                     c->state == CONTAINER_KILLED);
                    int ecode = c ? c->exit_code : 0;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    if (done) {
                        control_response_t final;
                        memset(&final, 0, sizeof(final));
                        final.status    = 0;
                        final.exit_code = ecode;
                        snprintf(final.message, sizeof(final.message),
                                 "DONE exit_code=%d", ecode);
                        write(client_fd, &final, sizeof(final));
                        break;
                    }
                    usleep(200000);
                }
            }
        }
        close(client_fd);
    }

    fprintf(stderr, "Supervisor shutting down...\n");

    /* Kill all remaining containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c;
    for (c = ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);
    pthread_mutex_lock(&ctx.metadata_lock);
    for (c = ctx.containers; c; c = c->next)
        kill(c->host_pid, SIGKILL);
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Join producer threads */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (c = ctx.containers; c; c = c->next) {
        if (c->producer_thread)
            pthread_join(c->producer_thread, NULL);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Shutdown logging */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

cleanup:
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_lock(&ctx.metadata_lock);
    free_container_list(ctx.containers);
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "Supervisor exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI Client — sends request, reads response                          */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int                 sock;
    struct sockaddr_un  addr;
    control_response_t  resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }

    /* Read first response */
    if (read(sock, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("read");
        close(sock);
        return 1;
    }

    /* Handle CMD_LOGS: read log file and print */
    if (req->kind == CMD_LOGS && resp.status == 0) {
        const char *prefix = "LOG_PATH:";
        if (strncmp(resp.message, prefix, strlen(prefix)) == 0) {
            const char *path = resp.message + strlen(prefix);
            FILE *f = fopen(path, "r");
            if (!f) {
                fprintf(stderr, "Could not open log file: %s\n", path);
                close(sock);
                return 1;
            }
            char line[256];
            while (fgets(line, sizeof(line), f))
                fputs(line, stdout);
            fclose(f);
            close(sock);
            return 0;
        }
    }

    printf("%s\n", resp.message);

    /* CMD_RUN: wait for final DONE response */
    if (req->kind == CMD_RUN && resp.status == 0) {
        if (read(sock, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
            printf("%s\n", resp.message);
            close(sock);
            return resp.exit_code;
        }
    }

    close(sock);
    return resp.status == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI Command Handlers                                                 */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind              = CMD_START;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind              = CMD_RUN;
    req.wait_for_exit     = 1;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
