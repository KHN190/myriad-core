# Myriad Runtime

Runtime only in C. Load and run a `.pk` cartridge. Wrote with spec as single source of truth. Used to prove bytecode design soundness.

```bash
# Either,
clang -O2 -Wall main.c -o myriad
# or
cc -O2 -Wall -Wextra -std=c99 -o myriad main.c
# And run
./myriad nqeeuns.pk
```

For Abrase compiler and cli see [here](https://github.com/KHN190/Abrase).
