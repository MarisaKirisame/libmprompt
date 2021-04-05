# Libmprompt Sources

- `main.c`: includes all needed sources in one file for easy inclusion into other projects.
- `mprompt.c`: the main multi-prompt delimited control interface. Uses the lower-level
   _gstacks_ and _longjmp_ assembly routines.
- `gstack.c`: in-place growable stacks using virtual memory. Provides the main interface
  to allocate gstacks and maintains a thread-local cache of gstacks.
  This file includes the  following files depending on the OS:
   - `gstack_gpool.c`: Implements an efficient virtual "pool" of gstacks that is needed
      on systems without overcommit to reliably (and efficiently) determine if an address can be 
      demand-paged.
   - `gstack_win.c`: gstacks using the Windows `VirtualAlloc` API.
   - `gstack_mmap.c`: gstacks using the Posix `mmap` API.
   - `gstack_mmap_mach.c`: included by `gstack_mmap.c` on macOS (using the Mach kernel) which
      implements a Mach exception handler to catch memory faults in a gstack (and handle them)
      before they get to the debugger.
- `util.c`: error messages.
- `asm`: platform specific assembly routines to switch efficiently between stacks:
   - `asm/longjmp_amd64_win.asm`: for Windows amd64/x84_64.
   - `asm/longjmp_amd64.S`: the AMD64 System-V ABI (Linux, macOS, etc).
   - `asm/longjmp_arm64.S`: the Aarch64 ABI (ARM64).


# Low-level layout of gstacks

## Windows

On Windows, a gstack is allocated as:

```ioke
|------------|
| xxxxxxxxxx | <-- noaccess gap (64 KiB by default)
|------------| <-- base
| committed  |
| ...        | <-- sp
|------------|
| guard page |
|------------|
| reserved   | (committed on demand)
| ...        |
.            .
.            .
|------------| <-- limit 
| xxxxxxxxxx | 
|------------| <-- 8MiB by default
```
The guard page at the end of the committed area will
move down into the reserved area to commit further stack pages on-demand. 

If enabling gpools ([`config.gpool_enable`](test/main.c#L28)), the layout 
of the stack is the same but there are two differences: (1) the stacks will 
grow more aggressive doubling the committed area every time (up to 1MiB) which 
can help performance, and (2), the stack memory is reused in the process which 
can be more efficient than allocating from the OS from scratch (which needs to 
re-zero pages for example).

## Linux and macOS

On `mmap` based systems the layout depends whether gpools
are enabled. If gpools are enabled (which is automatic of the
OS has no overcommit), the layout is:

```ioke
|------------|
| xxxxxxxxxx |
|------------| <-- base
| committed  |
| ...        | <-- sp
|------------|
| reserved   | (committed on demand)
|            |
.            .
.            .
|------------| <-- limit
| xxxxxxxxxx |
|------------|
```

The `reserved` space is committed on-demand using a signal
handler where the gpool allows the handler to determine
reliably whether a stack can be grown safely (up to the `limit`). 
(As described earlier, this also allows a stack in a gpool to 
grow through doubling which can be more performant, as well as 
allow better reuse of allocated stack memory.)

If the OS has overcommit (and gpools are not enabled explicitly),
the gstack is allocated instead as fully committed from the start
(with read/write access):


```ioke
|------------|
| xxxxxxxxxx |
|------------| <-- base
| committed  |
| ...        | <-- sp
|            |
|            |
.            .
.            .
|------------| <-- limit
| xxxxxxxxxx |
|------------|
```

This is simpler than gpools, as no signal handler is required.
However it will count 8MiB for each stack against the virtual commit count,
even though the actual physical pages are only committed on-demand
by the OS. This may lead to trouble if 
the [overcommit limit](https://www.kernel.org/doc/Documentation/vm/overcommit-accounting) 
is set too low.

   