# Myriad Runtime

Runtime only in C. Wrote with spec as single source of truth. Used to prove bytecode design completeness.

```bash
# Either,
clang -O2 -Wall main.c -o myriad
# or
cc -O2 -Wall -Wextra -std=c99 -o myriad main.c
# And run
./abrase-mini nqeeuns.pk
```

It runs .pk cartridges. For Abrase compiler and cli see [here](https://github.com/KHN190/Abrase).
