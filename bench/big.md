# Bigger-binary exec latency

Host `Linux 7.1.2 x86_64`; hyperfine -N warmup=10 min-runs=60. mean ± stddev (ms).

| subject | libs | ELF size | SELF size | ELF | memfd (M1) | native (M2) |
|---|--:|--:|--:|--:|--:|--:|
| hello | 2 | 15 KiB | 56 KiB | 1.401 ± 0.303 | 6.836 ± 0.586 | 6.915 ± 0.678 |
| git | 5 | 4642 KiB | 10028 KiB | 3.000 ± 0.311 | 32.714 ± 1.232 | 20.821 ± 3.040 |
| curl | 27 | 274 KiB | 684 KiB | 11.069 ± 2.669 | 18.751 ± 2.382 | 15.170 ± 1.014 |
| gdb | 47 | 41061 KiB | 95940 KiB | 86.109 ± 3.248 | 196.525 ± 4.615 | 156.414 ± 4.492 |
