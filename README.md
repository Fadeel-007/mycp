# mycp — a simplified file copy utility

A POSIX-portable reimplementation of `cp` for the course
*Advanced Programming in the UNIX Environment*, covering APUE chapters 3, 4, and 5.

## Build

```
make
```

This produces a single binary, `./mycp`. Tested on Linux with `gcc` and on macOS with `clang`.

## Usage

```
mycp [-pnsSbv] source dest
```

| Flag | Effect                                                                  |
|------|-------------------------------------------------------------------------|
| `-p` | preserve permissions (and owner/group when running as root)             |
| `-n` | no-clobber; refuse to overwrite an existing destination (atomic, O_EXCL)|
| `-s` | create a symbolic link instead of copying data                          |
| `-S` | sparse-aware copy: detect zero regions and recreate them as holes       |
| `-b` | buffered copy using the C stdio library (`fopen`/`fread`/`fwrite`)      |
| `-v` | verbose; print a status line on success                                 |

Examples:

```
./mycp file.txt copy.txt              # plain copy
./mycp -n file.txt copy.txt           # only if copy.txt doesn't exist
./mycp -p file.txt copy.txt           # keep original permissions
./mycp -s /etc/passwd /tmp/link       # make a symlink, no data copied
./mycp -S sparse.img backup.img       # preserve holes in sparse files
./mycp -bv file.txt copy.txt          # buffered + verbose
```

## Implementation notes

- The basic copy uses a 4096-byte buffer with a partial-write loop, so it
  correctly handles the case where `write()` writes fewer bytes than requested
  or returns `EINTR`.
- `-n` is implemented with `O_EXCL` rather than a check-then-open sequence,
  to avoid the well-known TOCTOU race condition.
- `-p` uses `fchmod`/`fchown` on the open destination fd (not `chmod`/`chown`
  on the path) to remove a small race window between `close()` and the
  permission change.
- `-s` always refuses to overwrite an existing destination, regardless of `-n`,
  because there is no portable atomic "create symlink only if absent" call.
  Existence is checked with `lstat` so that an existing symlink is detected
  even if its target is missing.
- `-S` decides a file is sparse by comparing `st_blocks * 512` against
  `st_size`. Each chunk read is scanned for all-zeros; if so, the destination
  is advanced with `lseek(SEEK_CUR)` instead of being written. After the loop,
  `ftruncate()` fixes the final size in case the file ends in a hole.
- `-b` uses `fdopen()` on a destination fd that was opened with the right
  `O_EXCL`/`O_TRUNC` flags, so `-b` and `-n` compose correctly — something
  plain `fopen()` cannot express on its own. `setvbuf()` is called with our
  own `BUFFSIZE` buffer in `_IOFBF` mode so the buffering is explicit and
  inspectable.
- `stderr` is set to line-buffered in `main()` so that error and verbose
  messages appear immediately rather than being held in a stdio buffer.

## Benchmark: `-b` vs raw I/O

Test: copy a 200 MB file of random data from `/tmp` to `/tmp` (same filesystem,
warm page cache after the first run), three runs each, `BUFFSIZE = 4096`.

| Run     | Raw (`read`/`write`) | Buffered (`-b`, `fread`/`fwrite`) |
|---------|----------------------|------------------------------------|
| 1       | 1.016 s (cold cache) | 0.130 s                            |
| 2       | 0.145 s              | 0.123 s                            |
| 3       | 0.119 s              | 0.129 s                            |

**Findings.** With a 4 KB buffer the two modes are essentially tied on a warm
page cache (~0.12 s for 200 MB). The first raw run is slower only because the
page cache was cold; subsequent runs match the buffered version. This is the
expected result: stdio's buffering helps most when the application makes many
*small* reads or writes, because it batches them into one syscall. When the
application is already calling `read`/`write` with a 4096-byte buffer, each
syscall is large enough that the extra stdio layer adds nothing — and on a
short hot file it actually adds a tiny amount of memcpy overhead. The win for
`-b` would only show up clearly if our application read or wrote a few bytes
at a time (e.g. character-by-character with `fgetc`), where stdio would
collapse thousands of tiny calls into a handful of real syscalls.

## Required short comments

**(1) Why does sparseness matter, and how does our `-S` flag preserve it?**
Sparse files store long runs of zeros as holes that take no disk blocks; the
filesystem just remembers the gap. A naive `read`/`write` loop reads those
zeros into a buffer and writes them out as real data, inflating the copy
from a few kilobytes to potentially gigabytes. Our `-S` implementation scans
each chunk for all-zeros and, when it finds one, advances the destination
with `lseek(SEEK_CUR)` instead of calling `write()`. Because `lseek()` past
end-of-file does not allocate blocks, the destination ends up with a hole in
the same place as the source. A final `ftruncate()` to `st_size` covers the
case where the source ends in a hole, since otherwise our last `lseek` would
leave the destination shorter than the source.

**(2) When is buffered I/O (`-b`) actually beneficial, and when is it not?**
Stdio buffering wins whenever the program performs many small reads or writes,
because it amortises the cost of syscalls (and on Linux, the cost of crossing
the user/kernel boundary) across one larger transfer. For typical
character-oriented code — `fgetc`, `fputc`, line-based parsing — the speedup
can be one or two orders of magnitude. It is *not* beneficial when the
program already does block-sized I/O: in this assignment we use a 4096-byte
buffer in both modes, so each raw `read`/`write` is already one syscall per
4 KB and stdio cannot reduce that count further. In that regime `-b` adds an
extra in-process memcpy with no syscall savings, so the two modes perform
identically (as our benchmark confirms).

## Files

```
mycp/
├── mycp.c       all source code
├── Makefile     `make` builds `./mycp`
├── README.md    this file
└── .gitignore   excludes the compiled binary
```