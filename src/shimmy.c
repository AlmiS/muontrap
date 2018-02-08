#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

#ifdef DEBUG
static FILE *debugfp = NULL;
#define INFO(MSG, ...) do { fprintf(debugfp, MSG, ## __VA_ARGS__); fflush(debugfp); } while (0)
#else
#define INFO(MSG, ...) ;
#endif

// asprintf can fail, but it's so rare that it's annoying to see the checks in the code.
#define checked_asprintf(MSG, ...) do { if (asprintf(MSG, ## __VA_ARGS__) < 0) err(EXIT_FAILURE, "asprintf"); } while (0)

static struct option long_options[] = {
    {"controller", required_argument, 0, 'c'},
    {"help",     no_argument,       0, 'h'},
    {"delay-to-sigkill", required_argument, 0, 'k'},
    {"path", required_argument, 0, 'p'},
    {"set", required_argument, 0, 's'},
    {"uid", required_argument, 0, 'u'},
    {"gid", required_argument, 0, 'g'},
    {0,          0,                 0, 0 }
};

#define CGROUP_MOUNT_PATH "/sys/fs/cgroup"

struct controller_var {
    struct controller_var *next;
    const char *key;
    const char *value;
};

struct controller_info {
    const char *name;
    char *path;
    char *procfile;

    struct controller_var *vars;
    struct controller_info *next;
};

static struct controller_info *controllers = NULL;
static const char *cgroup_path = NULL;
static int brutal_kill_wait_us = 1000;
static uid_t run_as_uid = 0; // 0 means don't set, since we don't support priviledge escalation
static gid_t run_as_gid = 0; // 0 means don't set, since we don't support priviledge escalation

static int signal_pipe[2] = { -1, -1};

#define FOREACH_CONTROLLER for (struct controller_info *controller = controllers; controller != NULL; controller = controller->next)

static void move_pid_to_cgroups(pid_t pid);

static void usage()
{
    printf("Usage: shimmy [OPTION] -- <program> <args>\n");
    printf("\n");
    printf("Options:\n");

    printf("--controller,-c <cgroup controller> (may be specified multiple times)\n");
    printf("--path,-p <cgroup path>\n");
    printf("--set,-s <cgroup variable>=<value>\n (may be specified multiple times)\n");
    printf("--delay-to-sigkill,-k <microseconds>\n");
    printf("--uid <uid/user> drop priviledge to this uid or user\n");
    printf("--gid <gid/group> drop priviledge to this gid or group\n");
    printf("-- the program to run and its arguments come after this\n");
}

void sigchild_handler(int signum)
{
    if (signal_pipe[1] >= 0 &&
            write(signal_pipe[1], &signum, sizeof(signum)) < 0)
        warn("write(signal_pipe)");
}

static int fork_exec(const char *path, char *const *argv)
{
    INFO("Running %s\n", path);
    for (char *const *arg = argv; *arg != NULL; arg++) {
        INFO("  arg: %s\n", *arg);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // child

        // Move to the container
        move_pid_to_cgroups(getpid());

        // Drop/change privilege if requested
        // See https://wiki.sei.cmu.edu/confluence/display/c/POS36-C.+Observe+correct+revocation+order+while+relinquishing+privileges
        if (run_as_gid > 0 && setgid(run_as_gid) < 0)
            err(EXIT_FAILURE, "setgid(%d)", run_as_gid);

        if (run_as_uid > 0 && setuid(run_as_uid) < 0)
            err(EXIT_FAILURE, "setuid(%d)", run_as_uid);

        execvp(path, argv);

        // Not supposed to reach here.
        exit(EXIT_FAILURE);
    } else {

        return pid;
    }
}

static int mkdir_p(const char *abspath, int start_index)
{
    int rc = 0;
    int last_errno = 0;
    char *path = strdup(abspath);
    for (int i = start_index; ; i++) {
        if (path[i] == '/' || path[i] == 0) {
            char save = path[i];
            path[i] = 0;
            rc = mkdir(path, 0755);
            if (rc < 0)
                last_errno = errno;

            path[i] = save;
            if (save == 0)
                break;
        }
    }
    free(path);

    // Return the last call to mkdir since that's the one that matters
    // and earlier directories are likely already created.
    errno = last_errno;
    return rc;
}

static void create_cgroups()
{
    FOREACH_CONTROLLER {
        int start_index = strlen(CGROUP_MOUNT_PATH) + 1 + strlen(controller->name) + 1;
        INFO("Create cgroup: mkdir -p %s\n", controller->path);
        if (mkdir_p(controller->path, start_index) < 0) {
            if (errno == EEXIST)
                errx(EXIT_FAILURE, "'%s' already exists. Please specify a deeper path or clean up the cgroup",
                     controller->path);
            else
                err(EXIT_FAILURE, "Couldn't create '%s'. Check permissions.", controller->path);
        }
    }
}

static int write_file(const char *path, const char *value)
{
   FILE *fp = fopen(path, "w");
   if (!fp)
       return -1;

   int rc = fwrite(value, 1, strlen(value), fp);
   fclose(fp);
   return rc;
}

static void update_cgroup_settings()
{
    FOREACH_CONTROLLER {
        for (struct controller_var *var = controller->vars;
             var != NULL;
             var = var->next) {
            char *setting_file;
            checked_asprintf(&setting_file, "%s/%s", controller->path, var->key);
            if (write_file(setting_file, var->value) < 0)
                err(EXIT_FAILURE, "Error writing '%s' to '%s'", var->value, setting_file);
            free(setting_file);
        }
    }
}

static void move_pid_to_cgroups(pid_t pid)
{
    FOREACH_CONTROLLER {
        FILE *fp = fopen(controller->procfile, "w");
        if (fp == NULL ||
            fprintf(fp, "%d", pid) < 0)
            err(EXIT_FAILURE, "Can't add pid to %s", controller->procfile);
        fclose(fp);
    }
}

static void destroy_cgroups()
{
    FOREACH_CONTROLLER {
        // Only remove the final directory, since we don't keep track of
        // what we actually create.
        INFO("rmdir %s\n", controller->path);
        rmdir(controller->path);
    }
}

static void procfile_killall(const char *path, int sig)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;

    int pid;
    while (fscanf(fp, "%d", &pid) == 1) {
        INFO("  kill -%d %d\n", sig, pid);
        kill(pid, sig);
    }
    fclose(fp);
}

static int procfile_has_processes(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    int pid;
    int rc = fscanf(fp, "%d", &pid);
    fclose(fp);

    return rc == 1;
}

static void kill_children(int sig)
{
    FOREACH_CONTROLLER {
        INFO("killall -%d from %s\n", sig, controller->procfile);
        procfile_killall(controller->procfile, sig);
    }
}

static void finish_controller_init()
{
    FOREACH_CONTROLLER {
        checked_asprintf(&controller->path, "%s/%s/%s", CGROUP_MOUNT_PATH, controller->name, cgroup_path);
        checked_asprintf(&controller->procfile, "%s/cgroup.procs", controller->path);
    }
}

static int child_processes_exist()
{
    FOREACH_CONTROLLER {
        if (procfile_has_processes(controller->procfile))
            return 1;
    }
    return 0;
}

static void cleanup()
{
    INFO("cleaning up!\n");

    // Turn off signal handlers
    sigaction(SIGCHLD, NULL, NULL);
    sigaction(SIGINT, NULL, NULL);
    sigaction(SIGQUIT, NULL, NULL);
    sigaction(SIGTERM, NULL, NULL);

    // If the subprocess responded to our SIGTERM, then hopefully
    // nothing exists, but if subprocesses do exist, repeatedly
    // kill them until they all go away.
    int retries = 10;
    while (retries > 0 && child_processes_exist()) {
        kill_children(SIGKILL);
        usleep(1000);
        retries--;
    }

    if (retries == 0) {
        // Hammer the child processes as a final attempt (no waiting this time)
        retries = 10;
        while (retries > 0 && child_processes_exist()) {
            kill_children(SIGKILL);
            retries--;
        }

        if (retries == 0)
            warnx("Failed to kill all children even after retrying!");
    }

    // Clean up our cgroup
    destroy_cgroups();

    INFO("cleanup done\n");
}

static void kill_child_nicely(pid_t child)
{
    // Start with SIGTERM
    kill(child, SIGTERM);

    // Wait a little.
    if (brutal_kill_wait_us > 0)
        usleep(brutal_kill_wait_us);

    // Brutal kill
    kill(child, SIGKILL);
}

static struct controller_info *add_controller(const char *name)
{
    struct controller_info *new_controller = malloc(sizeof(struct controller_info));
    new_controller->name = name;
    new_controller->path = NULL;
    new_controller->vars = NULL;
    new_controller->next = controllers;
    controllers = new_controller;

    return new_controller;
}

static void add_controller_setting(struct controller_info *controller, const char *key, const char *value)
{
    struct controller_var *new_var = malloc(sizeof(struct controller_var));
    new_var->key = key;
    new_var->value = value;
    new_var->next = controller->vars;
    controller->vars = new_var;
}

int main(int argc, char *argv[])
{
#ifdef DEBUG
    debugfp = fopen("shimmy-debug.log", "w");
    if (!debugfp)
        debugfp = stderr;
#endif
    if (argc == 1) {
        usage();
        exit(EXIT_FAILURE);
    }

    int opt;
    struct controller_info *current_controller = NULL;
    while ((opt = getopt_long(argc, argv, "c:hp:s:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            if (cgroup_path)
                errx(EXIT_FAILURE, "Only one cgroup path supported.");
            cgroup_path = optarg;
            break;

        case 'c':
            current_controller = add_controller(optarg);
            break;

        case 's':
        {
            if (!current_controller)
                errx(EXIT_FAILURE, "Specify a cgroup controller (-c) before setting a variable");

            char *equalsign = strchr(optarg, '=');
            if (!equalsign)
                errx(EXIT_FAILURE, "No '=' found when setting a variable: '%s'", optarg);

            // NULL terminate the key. We can do this since we're already modifying
            // the arguments by using getopt.
            *equalsign = '\0';
            add_controller_setting(current_controller, optarg, equalsign + 1);
            break;
        }

        case 'k': // --delay-to-sigkill
            brutal_kill_wait_us = strtoul(optarg, NULL, 0);
            if (brutal_kill_wait_us > 1000000)
                errx(EXIT_FAILURE, "Delay to sending a SIGKILL must be < 1,000,000 (1 second)");
            break;

        case 'g': // --gid
        {
            char *endptr;
            run_as_gid = strtoul(optarg, &endptr, 0);
            if (*endptr != '\0') {
                struct group *group = getgrnam(optarg);
                if (!group)
                    errx(EXIT_FAILURE, "Unknown group '%s'", optarg);
                run_as_gid = group->gr_gid;
            }
            if (run_as_gid == 0)
                errx(EXIT_FAILURE, "Setting the group to root or gid 0 is not allowed");
            break;
        }

        case 'u': // --uid
        {
            char *endptr;
            run_as_uid = strtoul(optarg, &endptr, 0);
            if (*endptr != '\0') {
                struct passwd *passwd = getpwnam(optarg);
                if (!passwd)
                    errx(EXIT_FAILURE, "Unknown user '%s'", optarg);
                run_as_uid = passwd->pw_uid;
            }
            if (run_as_uid == 0)
                errx(EXIT_FAILURE, "Setting the user to root or uid 0 is not allowed");
            break;
        }

        case 'h':
            usage();
            exit(EXIT_SUCCESS);

        default:
            usage();
            exit(EXIT_FAILURE);
        }
    }

    if (argc == optind)
        errx(EXIT_FAILURE, "Specify a program to run");

    if (cgroup_path == NULL && controllers)
        errx(EXIT_FAILURE, "Specify a cgroup path (-p)");

    if (cgroup_path && !controllers)
        errx(EXIT_FAILURE, "Specify a cgroup controller (-c) if you specify a path");

    finish_controller_init();

    if (pipe(signal_pipe) < 0)
        err(EXIT_FAILURE, "pipe");

    /* Set up the structure to specify the new action. */
    struct sigaction sa;
    sa.sa_handler = sigchild_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    atexit(cleanup);

    create_cgroups();

    update_cgroup_settings();

    pid_t pid = fork_exec(argv[optind], &argv[optind]);
    struct pollfd fds[3];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLHUP; // POLLERR is implicit
    fds[1].fd = signal_pipe[0];
    fds[1].events = POLLIN;
    fds[2].fd = STDOUT_FILENO;
    fds[2].events = POLLHUP; // POLLERR is implicit

    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;

            err(EXIT_FAILURE, "poll");
        }

        if (fds[0].revents) {
            INFO("stdin closed. cleaning up...");
            kill_child_nicely(pid);
            break;
        }
        if (fds[2].revents) {
            INFO("stdout closed. cleaning up...");
            kill_child_nicely(pid);
            break;
        }
        if (fds[1].revents) {
            int signal;
            ssize_t amt = read(signal_pipe[0], &signal, sizeof(signal));
            if (amt < 0)
                err(EXIT_FAILURE, "read signal_pipe");

            switch (signal) {
            case SIGCHLD: {
                int status;
                pid_t dying_pid = wait(&status);
                if (dying_pid == pid) {
                    int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
                    INFO("main child exited. status=%d, our exit code: %d\n", status, exit_status);
                    exit(exit_status);
                } else {
                    INFO("something else caused sigchild: pid=%d, status=%d. our child=%d\n", dying_pid, status, pid);
                }
                break;
            }

            case SIGTERM:
            case SIGQUIT:
            case SIGINT:
                exit(EXIT_FAILURE);

            default:
                err(EXIT_FAILURE, "unexpected signal: %d", signal);
            }
        }
    }

    exit(EXIT_SUCCESS);
}
