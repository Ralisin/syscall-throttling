# Sensitive syscall examples

These examples exercise calls with stronger effects on process state,
synchronization, networking, memory protection, or filesystem contents. Every
workload confines its effects to resources that it creates itself.

| Directory | Effect | Safety boundary | Default `MAX` |
|---|---|---|---:|
| `unlinkat` | Removes directory entries | Files in a fresh private `/tmp` directory | 2 |
| `mprotect` | Changes page permissions | One private anonymous mapping | 3 |
| `futex` | Blocks and wakes threads | One process-private futex word | 4 |
| `connect` | Opens network connections | Ephemeral listener on IPv4 loopback | 2 |
| `kill` | Delivers signals | Child processes created by the workload | 2 |
| `execve` | Replaces a process image | Child processes executing `/bin/true` | 2 |

Build and load the module, then configure and run one case:

```sh
make
sudo insmod kernel/syscall_throttle.ko
sudo ./tests/syscalls/sensitive/unlinkat/configure.sh
./tests/syscalls/sensitive/unlinkat/st_unlinkat
./user/throttle_ctl show
```

The optional argument to `configure.sh` overrides `MAX`. The optional argument
to a workload changes its invocation count. Every configuration uses
`--clear`, registers only the workload name, and resets statistics.

Run the complete isolated group with:

```sh
sudo ./scripts/test-sensitive-syscalls.sh
```
