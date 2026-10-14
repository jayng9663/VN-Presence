# `/proc/[PID]/stat` Field Reference

The `/proc/[PID]/stat` file exposes process status information. Each field is listed below in order of appearance.

## Fields

| ID | Field | Description |
|----|-------|-------------|
| 1 | `pid` | Process ID |
| 2 | `tcomm` | Filename of the executable |
| 3 | `state` | Process state: `R` running, `S` sleeping, `D` uninterruptible sleep, `Z` zombie, `T` traced or stopped |
| 4 | `ppid` | Process ID of the parent process |
| 5 | `pgrp` | Process group ID |
| 6 | `sid` | Session ID |
| 7 | `tty_nr` | TTY the process uses |
| 8 | `tty_pgrp` | Process group ID of the TTY |
| 9 | `flags` | Task flags |
| 10 | `min_flt` | Number of minor faults |
| 11 | `cmin_flt` | Number of minor faults including children |
| 12 | `maj_flt` | Number of major faults |
| 13 | `cmaj_flt` | Number of major faults including children |
| 14 | `utime` | User mode jiffies |
| 15 | `stime` | Kernel mode jiffies |
| 16 | `cutime` | User mode jiffies including children |
| 17 | `cstime` | Kernel mode jiffies including children |
| 18 | `priority` | Priority level |
| 19 | `nice` | Nice level |
| 20 | `num_threads` | Number of threads |
| 21 | `it_real_value` | _(Obsolete, always 0)_ |
| 22 | `start_time` | Time the process started after system boot |
| 23 | `vsize` | Virtual memory size |
| 24 | `rss` | Resident set memory size |
| 25 | `rsslim` | Current limit in bytes on the RSS |
| 26 | `start_code` | Address above which program text can run |
| 27 | `end_code` | Address below which program text can run |
| 28 | `start_stack` | Address of the start of the main process stack |
| 29 | `esp` | Current value of ESP |
| 30 | `eip` | Current value of EIP |
| 31 | `pending` | Bitmap of pending signals |
| 32 | `blocked` | Bitmap of blocked signals |
| 33 | `sigign` | Bitmap of ignored signals |
| 34 | `sigcatch` | Bitmap of caught signals |
| 35 | _(placeholder)_ | _(Was wchan address — use `/proc/[PID]/wchan` instead)_ |
| 36 | _(placeholder)_ | _(Reserved)_ |
| 37 | _(placeholder)_ | _(Reserved)_ |
| 38 | `exit_signal` | Signal sent to the parent thread on exit |
| 39 | `task_cpu` | CPU the task is scheduled on |
| 40 | `rt_priority` | Realtime priority |
| 41 | `policy` | Scheduling policy (see `man sched_setscheduler`) |
| 42 | `blkio_ticks` | Time spent waiting for block I/O |
| 43 | `gtime` | Guest time of the task in jiffies |
| 44 | `cgtime` | Guest time of child tasks in jiffies |
| 45 | `start_data` | Address above which program data+BSS is placed |
| 46 | `end_data` | Address below which program data+BSS is placed |
| 47 | `start_brk` | Address above which the heap can be expanded with `brk()` |
| 48 | `arg_start` | Address above which the command line is placed |
| 49 | `arg_end` | Address below which the command line is placed |
| 50 | `env_start` | Address above which the environment is placed |
| 51 | `env_end` | Address below which the environment is placed |
| 52 | `exit_code` | Thread exit code as reported by `waitpid` |

## Notes

- Fields are space-separated and appear in the order listed above.
- Time values (`utime`, `stime`, etc.) are measured in **jiffies** (clock ticks). Divide by `sysconf(_SC_CLK_TCK)` to convert to seconds.

## References

- [`The /proc Filesystem` linux kernel doc](https://www.kernel.org/doc/html/latest/filesystems/proc.html#id12)
- Linux kernel source: `fs/proc/array.c`
