/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <spawn.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

#if defined(__linux__) || defined(__GLIBC__)
# include <grp.h>
#endif


static void uv__chld(uv_signal_t* handle, int signum) {
  uv_process_t* process;
  uv_loop_t* loop;
  int exit_status;
  int term_signal;
  int status;
  pid_t pid;
  QUEUE pending;
  QUEUE* q;
  QUEUE* h;

  assert(signum == SIGCHLD);

  QUEUE_INIT(&pending);
  loop = handle->loop;

  h = &loop->process_handles;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    do
      pid = waitpid(process->pid, &status, WNOHANG);
    while (pid == -1 && errno == EINTR);

    if (pid == 0)
      continue;

    if (pid == -1) {
      if (errno != ECHILD)
        abort();
      continue;
    }

    process->status = status;
    QUEUE_REMOVE(&process->queue);
    QUEUE_INSERT_TAIL(&pending, &process->queue);
  }

  h = &pending;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    QUEUE_REMOVE(&process->queue);
    QUEUE_INIT(&process->queue);
    uv__handle_stop(process);

    if (process->exit_cb == NULL)
      continue;

    exit_status = 0;
    if (WIFEXITED(process->status))
      exit_status = WEXITSTATUS(process->status);

    term_signal = 0;
    if (WIFSIGNALED(process->status))
      term_signal = WTERMSIG(process->status);

    process->exit_cb(process, exit_status, term_signal);
  }
  assert(QUEUE_EMPTY(&pending));
}

/*
 * Used for initializing stdio streams like options.stdin_stream. Returns
 * zero on success. See also the cleanup section in uv_spawn().
 */
static int uv__process_init_stdio(uv_stdio_container_t* container, int fds[2]) {
  int mask;
  int fd;

  mask = UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM;

  switch (container->flags & mask) {
  case UV_IGNORE:
    return 0;

  case UV_CREATE_PIPE:
    assert(container->data.stream != NULL);
    if (container->data.stream->type != UV_NAMED_PIPE)
      return UV_EINVAL;
    else
      return uv_socketpair(SOCK_STREAM, 0, fds, 0, 0);

  case UV_INHERIT_FD:
  case UV_INHERIT_STREAM:
    if (container->flags & UV_INHERIT_FD)
      fd = container->data.fd;
    else
      fd = uv__stream_fd(container->data.stream);

    if (fd == -1)
      return UV_EINVAL;

    fds[1] = fd;
    return 0;

  default:
    assert(0 && "Unexpected flags");
    return UV_EINVAL;
  }
}


static int uv__process_open_stream(uv_stdio_container_t* container,
                                   int pipefds[2]) {
  int flags;
  int err;

  if (!(container->flags & UV_CREATE_PIPE) || pipefds[0] < 0)
    return 0;

  err = uv__close(pipefds[1]);
  if (err != 0)
    abort();

  pipefds[1] = -1;
  uv__nonblock(pipefds[0], 1);

  flags = 0;
  if (container->flags & UV_WRITABLE_PIPE)
    flags |= UV_HANDLE_READABLE;
  if (container->flags & UV_READABLE_PIPE)
    flags |= UV_HANDLE_WRITABLE;

  return uv__stream_open(container->data.stream, pipefds[0], flags);
}


static void uv__process_close_stream(uv_stdio_container_t* container) {
  if (!(container->flags & UV_CREATE_PIPE)) return;
  uv__stream_close(container->data.stream);
}


static void uv__write_int(int fd, int val) {
  ssize_t n;

  do
    n = write(fd, &val, sizeof(val));
  while (n == -1 && errno == EINTR);

  if (n == -1 && errno == EPIPE)
    return; /* parent process has quit */

  assert(n == sizeof(val));
}


#if !(defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH))
/* execvp is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED, so must be
 * avoided. Since this isn't called on those targets, the function
 * doesn't even need to be defined for them.
 */
static void uv__process_child_init(const uv_process_options_t* options,
                                   int stdio_count,
                                   int (*pipes)[2],
                                   int error_fd) {
  sigset_t set;
  int close_fd;
  int use_fd;
  int err;
  int fd;
  int n;

  if (options->flags & UV_PROCESS_DETACHED)
    setsid();

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0 || use_fd >= fd)
      continue;
    pipes[fd][1] = fcntl(use_fd, F_DUPFD, stdio_count);
    if (pipes[fd][1] == -1) {
      uv__write_int(error_fd, UV__ERR(errno));
      _exit(127);
    }
  }

  for (fd = 0; fd < stdio_count; fd++) {
    close_fd = pipes[fd][0];
    use_fd = pipes[fd][1];

    if (use_fd < 0) {
      if (fd >= 3)
        continue;
      else {
        /* redirect stdin, stdout and stderr to /dev/null even if UV_IGNORE is
         * set
         */
        use_fd = open("/dev/null", fd == 0 ? O_RDONLY : O_RDWR);
        close_fd = use_fd;

        if (use_fd < 0) {
          uv__write_int(error_fd, UV__ERR(errno));
          _exit(127);
        }
      }
    }

    if (fd == use_fd)
      uv__cloexec_fcntl(use_fd, 0);
    else
      fd = dup2(use_fd, fd);

    if (fd == -1) {
      uv__write_int(error_fd, UV__ERR(errno));
      _exit(127);
    }

    if (fd <= 2)
      uv__nonblock_fcntl(fd, 0);

    if (close_fd >= stdio_count)
      uv__close(close_fd);
  }

  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];

    if (use_fd >= stdio_count)
      uv__close(use_fd);
  }

  if (options->cwd != NULL && chdir(options->cwd)) {
    uv__write_int(error_fd, UV__ERR(errno));
    _exit(127);
  }

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* When dropping privileges from root, the `setgroups` call will
     * remove any extraneous groups. If we don't call this, then
     * even though our uid has dropped, we may still have groups
     * that enable us to do super-user things. This will fail if we
     * aren't root, so don't bother checking the return value, this
     * is just done as an optimistic privilege dropping function.
     */
    SAVE_ERRNO(setgroups(0, NULL));
  }

  if ((options->flags & UV_PROCESS_SETGID) && setgid(options->gid)) {
    uv__write_int(error_fd, UV__ERR(errno));
    _exit(127);
  }

  if ((options->flags & UV_PROCESS_SETUID) && setuid(options->uid)) {
    uv__write_int(error_fd, UV__ERR(errno));
    _exit(127);
  }

  if (options->env != NULL) {
    environ = options->env;
  }

  /* Reset signal disposition.  Use a hard-coded limit because NSIG
   * is not fixed on Linux: it's either 32, 34 or 64, depending on
   * whether RT signals are enabled.  We are not allowed to touch
   * RT signal handlers, glibc uses them internally.
   */
  for (n = 1; n < 32; n += 1) {
    if (n == SIGKILL || n == SIGSTOP)
      continue;  /* Can't be changed. */

#if defined(__HAIKU__)
    if (n == SIGKILLTHR)
      continue;  /* Can't be changed. */
#endif

    if (SIG_ERR != signal(n, SIG_DFL))
      continue;

    uv__write_int(error_fd, UV__ERR(errno));
    _exit(127);
  }

  /* Reset signal mask. */
  sigemptyset(&set);
  err = pthread_sigmask(SIG_SETMASK, &set, NULL);

  if (err != 0) {
    uv__write_int(error_fd, UV__ERR(err));
    _exit(127);
  }

  execvp(options->file, options->args);
  uv__write_int(error_fd, UV__ERR(errno));
  _exit(127);
}
#endif


int uv_spawn(uv_loop_t* loop,
             uv_process_t* process,
             const uv_process_options_t* options) {
#if defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
  /* fork is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED. */
  return UV_ENOSYS;
#else
  int signal_pipe[2] = { -1, -1 };
  int pipes_storage[8][2];
  int (*pipes)[2];
  int stdio_count;
  ssize_t r;
  pid_t pid;
  int err;
  int exec_errorno;
  int i;
  int status;

  assert(options->file != NULL);
  assert(!(options->flags & ~(UV_PROCESS_DETACHED |
                              UV_PROCESS_SETGID |
                              UV_PROCESS_SETUID |
                              UV_PROCESS_WINDOWS_HIDE |
                              UV_PROCESS_WINDOWS_HIDE_CONSOLE |
                              UV_PROCESS_WINDOWS_HIDE_GUI |
                              UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS)));

  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  QUEUE_INIT(&process->queue);

  // Make at least 3 file descriptors avialable to the child
  // (stdin, stdout, stderr)
  stdio_count = options->stdio_count;
  if (stdio_count < 3)
    stdio_count = 3;

  // Try to use the pipes_storage as the store for pipes, 
  // but if more pipes are needed than there are already 
  // allocated, allocate a new 
  err = UV_ENOMEM;
  pipes = pipes_storage;
  if (stdio_count > (int) ARRAY_SIZE(pipes_storage))
    pipes = uv__malloc(stdio_count * sizeof(*pipes));

  if (pipes == NULL)
    goto error;

  for (i = 0; i < stdio_count; i++) {
    pipes[i][0] = -1;
    pipes[i][1] = -1;
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_init_stdio(options->stdio + i, pipes[i]);
    if (err)
      goto error;
  }

  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it EOFs or errors with EPIPE.
   */
  err = uv__make_pipe(signal_pipe, 0);
  if (err)
    goto error;

  uv_signal_start(&loop->child_watcher, uv__chld, SIGCHLD);

  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);

#if defined(__APPLE__)
  if (__builtin_available(macOS 10.15, *)) {
    posix_spawnattr_t attrs;
    err = posix_spawnattr_init(&attrs);
    if(err)
      goto error;

    if ((options->flags & UV_PROCESS_SETUID) && posix_spawnattr_set_uid_np(&attrs, options->uid)) {
      goto error;
    }
    if ((options->flags & UV_PROCESS_SETGID) && posix_spawnattr_set_gid_np(&attrs, options->gid)) {
      goto error;
    }

    // Set flags for spawn behavior 
    // 1) POSIX_SPAWN_CLOWEXEC_DEFAULT: (Apple Extension) All descriptors in t
    // he parent will be treated as if they had been created with O_CLOEXEC. 
    // The only fds that will be passed on to the child are those manipulated by the file actions
    // 2) POSIX_SPAWN_SETSIGDEF: signals mentioned in sawn-sigdeffault in the spawn attributes
    // will be reset to behave as their default
    // 3) POSIX_SPAWN_SETSIGMASK: signal mask will be set to the value of spaw-sigmask in attributes
    // 4) POSIX_SPAWN_SETSID: if process should be a new session leader, do that.
    err = posix_spawnattr_setflags(&attrs, 
      POSIX_SPAWN_CLOEXEC_DEFAULT |
      POSIX_SPAWN_SETSIGDEF |
      POSIX_SPAWN_SETSIGMASK |
      (options->flags & UV_PROCESS_DETACHED ? POSIX_SPAWN_SETSID : 0)
      );
    if(err)
      goto error;

    // Reset all signal the child to their default behavior
    sigset_t signal_set;
    sigfillset(&signal_set);
    err = posix_spawnattr_setsigdefault(&attrs, &signal_set);
    if(err) 
      goto error;

    // Reset the signal mask for all signals
    sigemptyset(&signal_set);
    err = posix_spawnattr_setsigmask(&attrs, &signal_set);
    if(err)
      goto error;

    posix_spawn_file_actions_t actions;
    err = posix_spawn_file_actions_init(&actions);
    if(err)
      goto error;

    if (options->cwd != NULL && posix_spawn_file_actions_addchdir_np(&actions, options->cwd))
        goto error;

    // Process the fd allocations
    for(int fd = 0 ; fd < options->stdio_count; ++fd) {
      const int mask = UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM;
      const int flags = options->stdio[fd].flags & mask;

      if(flags == UV_IGNORE) {
        asset(pipes[fd][1] = -1);
        if(fd < 3) {
          // When ignored, the standard streams are redirected to (or from)
          // /dev/null, per documentation. This is done by the child, and in the
          // fork() path it is done by uv__process_child_init
          const int oflags = fd == 0 ? O_RDONLY : O_RDWR;
          const int mode = 0;
          posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", oflags, mode);
        }
      } else if (flags == UV_INHERIT_FD || flags == UV_INHERIT_STREAM) {
        // The actual descriptors in this code path are set up by
        // uv__process_init_stdio

        // FIXME: this is broken. If the caller specified that the child
        // stdout is the parents stderr, and that the childs stderr is the 
        // parents stdout, this logic will
        //  * for fd = 1 (child stdout) will dup2(2, 1) <- duplicating parents 2 into 1
        //  * for fd = 2 (child stderr) will dup2(1, 2) <- duplicating parents 1 into 2
        // ...but since the first call had already rewritten 1, both will
        // end up pointing to the parent's 1.


        // Check if the inherited FD is already at the same fd that was 
        // expected for it in the child (the entry location in the stdio
        // table). If not, make sure it ends up there.
        if(fd == pipes[fd][1]) {
          // Make the original descriptor available in the child
          posix_spawn_file_actions_addinherit_np(&actions, pipes[fd][1]);
        } else {
          // Dup the inherited fd into the expected fd in the child
          posix_spawn_file_actions_adddup2(&actions, pipes[fd][1], fd);

          // ????????
          // FIXME: What do we do with the parent-inherited fd? Can't close 
          // if it is also inherited to other slot at least.          
        }        
      } else if (flags == UV_CREATE_PIPE) {
        // PIPEs
        //posix_spawn_file_actions_adddup2(&actions, )
      } else {
        fprintf(stderr, "Offending flags: 0x%x\n", flags);
        // This should not happen, because it would have failed first in 
        // uv__process_init_stdio
        assert(0 && "Unexpected flags");
        err = UV_EINVAL;
      }
    }

    // Spawn the child 
    err = posix_spawnp(&pid, options->file, &actions, &attrs, options->args, options->env);    



  } else {
#endif
    pid = fork();

    if (pid == -1) {
      err = UV__ERR(errno);
      uv_rwlock_wrunlock(&loop->cloexec_lock);
      uv__close(signal_pipe[0]);
      uv__close(signal_pipe[1]);
      goto error;
    }

    if (pid == 0) {
      uv__process_child_init(options, stdio_count, pipes, signal_pipe[1]);
      abort();
    }
#if defined(__APPLE__)
  }
#endif

  /* Release lock in parent process */
  uv_rwlock_wrunlock(&loop->cloexec_lock);
  uv__close(signal_pipe[1]);

  process->status = 0;
  exec_errorno = 0;
  do
    r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno));
  while (r == -1 && errno == EINTR);

  if (r == 0)
    ; /* okay, EOF */
  else if (r == sizeof(exec_errorno)) {
    do
      err = waitpid(pid, &status, 0); /* okay, read errorno */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else if (r == -1 && errno == EPIPE) {
    do
      err = waitpid(pid, &status, 0); /* okay, got EPIPE */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else
    abort();

  uv__close_nocheckstdio(signal_pipe[0]);

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_open_stream(options->stdio + i, pipes[i]);
    if (err == 0)
      continue;

    while (i--)
      uv__process_close_stream(options->stdio + i);

    goto error;
  }

  /* Only activate this handle if exec() happened successfully */
  if (exec_errorno == 0) {
    QUEUE_INSERT_TAIL(&loop->process_handles, &process->queue);
    uv__handle_start(process);
  }

  process->pid = pid;
  process->exit_cb = options->exit_cb;

  if (pipes != pipes_storage)
    uv__free(pipes);

  return exec_errorno;

error:
  if (pipes != NULL) {
    for (i = 0; i < stdio_count; i++) {
      if (i < options->stdio_count)
        if (options->stdio[i].flags & (UV_INHERIT_FD | UV_INHERIT_STREAM))
          continue;
      if (pipes[i][0] != -1)
        uv__close_nocheckstdio(pipes[i][0]);
      if (pipes[i][1] != -1)
        uv__close_nocheckstdio(pipes[i][1]);
    }

    if (pipes != pipes_storage)
      uv__free(pipes);
  }

  return err;
#endif
}


int uv_process_kill(uv_process_t* process, int signum) {
  return uv_kill(process->pid, signum);
}


int uv_kill(int pid, int signum) {
  if (kill(pid, signum))
    return UV__ERR(errno);
  else
    return 0;
}


void uv__process_close(uv_process_t* handle) {
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  if (QUEUE_EMPTY(&handle->loop->process_handles))
    uv_signal_stop(&handle->loop->child_watcher);
}
