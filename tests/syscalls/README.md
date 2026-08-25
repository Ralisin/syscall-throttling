# Syscall examples

Each subdirectory contains a small workload and the matching monitor
configuration. The examples cover fast identity calls, a side-effecting call,
filesystem access, and a naturally blocking call.

Build everything from the repository root and load the module once:

```sh
make
sudo insmod kernel/syscall_throttle.ko
```

Then configure and run one example. For instance:

```sh
sudo ./tests/syscalls/getuid/configure.sh
./tests/syscalls/getuid/st_getuid 7
./user/throttle_ctl show
```

The optional argument to every `configure.sh` is `MAX`; its default is shown
in the table. Every configuration starts with `--clear`, so examples do not
inherit registrations or statistics from a previous run.

| Directory | Registered syscall | Default `MAX` | Workload |
|---|---:|---:|---|
| `getpid` | `getpid` | 3 | Seven direct identity calls |
| `getuid` | `getuid` | 3 | Seven direct identity calls |
| `write` | `write` | 3 | Seven one-byte writes to `/dev/null` |
| `openat` | `openat` | 2 | Five open/close cycles on `/dev/null` |
| `read` | `read` | 2 | Five pipe reads with a delayed writer |

Each workload accepts the number of calls as its optional first argument. The
examples invoke `syscall(2)` directly so the intended syscall is unambiguous.
The `openat` result can include calls made by the dynamic loader before
`main`; this is valid monitor activity but makes that example less
deterministic than `getpid` or `getuid`.

The `read` example intentionally combines two independent delays: the monitor
may defer admission, and the admitted syscall may then wait for pipe data. It
demonstrates that the monitor also handles naturally blocking syscalls.

When finished:

```sh
sudo ./user/throttle_ctl clear
sudo rmmod syscall_throttle
```
