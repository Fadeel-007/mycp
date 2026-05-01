/*
 * mycp.c - A feature-rich file copy utility
 *
 * APUE Assignment: Chapters 3, 4, and 5
 *
 * Benchmark results (fill in after Milestone 5):
 *   Raw syscall copy of 50MB:  _____ seconds
 *   stdio buffered copy (-b):  _____ seconds
 *   Observation: ...
 *
 * Compile: gcc -Wall -Wextra -pedantic -o mycp mycp.c
 */

/* ============================================================
 * HEADERS — These are like "toolboxes" we borrow from the OS
 * ============================================================ */
#include <sys/stat.h>   /* stat, chmod, lstat                  */
#include <sys/types.h>  /* extra type definitions               */
#include <fcntl.h>      /* open(), and flags like O_RDONLY      */
#include <unistd.h>     /* read(), write(), close(), unlink()   */
#include <stdio.h>      /* fprintf(), fopen(), fread(), fwrite()*/
#include <stdlib.h>     /* malloc(), free(), exit()             */
#include <string.h>     /* strerror() — turns error codes to text */
#include <errno.h>      /* errno — the global error number      */

/* ============================================================
 * BUFFSIZE — How many bytes we read/write at a time.
 * Think of it as the size of our "bucket" when moving water.
 * 4096 bytes = 4 KB, which matches the OS page size (efficient).
 * ============================================================ */
#define BUFFSIZE 4096

/* ============================================================
 * OPTION FLAGS — These are global variables that store which
 * flags the user passed in (like -p, -n, -v, etc.)
 * They start as 0 (off) and get set to 1 if the user uses them.
 * ============================================================ */
static int opt_preserve  = 0;  /* -p : preserve permissions    */
static int opt_noclobber = 0;  /* -n : don't overwrite dest    */
static int opt_symlink   = 0;  /* -s : create a symlink        */
static int opt_sparse    = 0;  /* -S : sparse-aware copy       */
static int opt_buffered  = 0;  /* -b : use stdio (fread/fwrite)*/
static int opt_verbose   = 0;  /* -v : print status after copy */

/* ============================================================
 * FORWARD DECLARATIONS — telling C "these functions exist,
 * I'll define them below." Required because C reads top-down.
 * ============================================================ */
static void parse_args(int argc, char *argv[],
                       const char **src, const char **dst);
static void copy_raw(const char *src, const char *dst);
static void copy_stdio(const char *src, const char *dst);
static void copy_symlink(const char *src, const char *dst);
static void die(const char *msg);
static void cleanup_and_die(const char *dst, const char *msg);

/* ============================================================
 * BYTES_COPIED — shared counter for verbose mode (-v).
 *
 * copy_raw() and copy_stdio() both update this as they work.
 * main() reads it at the end to print the status line.
 * Using off_t (not int) because files can be larger than 2GB.
 * ============================================================ */
static off_t bytes_copied = 0;


/* ============================================================
 * MAIN — The entry point. Program starts here.
 * argc = number of arguments, argv = the arguments themselves
 * Example: ./mycp -v file1.txt file2.txt
 *   argc = 4
 *   argv[0] = "./mycp"
 *   argv[1] = "-v"
 *   argv[2] = "file1.txt"
 *   argv[3] = "file2.txt"
 * ============================================================ */
int
main(int argc, char *argv[])
{
    const char *src, *dst;

    /* TODO Milestone 5: set stderr to line-buffered here */

    /* parse_args will fill in src and dst, and set the opt_ flags */
    parse_args(argc, argv, &src, &dst);

    /* Choose which operation to perform based on flags */
    if (opt_symlink) {
        copy_symlink(src, dst);     /* Milestone 3 */
    } else if (opt_buffered) {
        copy_stdio(src, dst);       /* Milestone 5 */
    } else {
        copy_raw(src, dst);         /* Milestone 1/2/4 */
    }

    /* TODO Milestone 5: print verbose status line */
    if (opt_verbose)
        ;

    return 0;  /* 0 = success */
}


/* ============================================================
 * PARSE_ARGS — Reads the command-line flags and file names.
 *
 * getopt() is a standard function that scans argv for flags.
 * The string "pnsSbv" tells it which letters are valid flags.
 * It returns the letter found, or -1 when there are no more.
 * After the loop, argv[optind] and argv[optind+1] are src/dst.
 * ============================================================ */
static void
parse_args(int argc, char *argv[], const char **src, const char **dst)
{
    int ch;

    /* getopt loops through all flags like -p, -v, -n, etc. */
    while ((ch = getopt(argc, argv, "pnsSbv")) != -1) {
        switch (ch) {
        case 'p': opt_preserve  = 1; break;
        case 'n': opt_noclobber = 1; break;
        case 's': opt_symlink   = 1; break;
        case 'S': opt_sparse    = 1; break;
        case 'b': opt_buffered  = 1; break;
        case 'v': opt_verbose   = 1; break;
        default:
            /* Unknown flag — print usage and quit */
            fprintf(stderr, "Usage: mycp [-pnsSbv] source dest\n");
            exit(1);
        }
    }

    /* After getopt, optind points to the first non-flag argument.
     * We need exactly 2 remaining: source and destination.        */
    if (argc - optind != 2) {
        fprintf(stderr, "Usage: mycp [-pnsSbv] source dest\n");
        exit(1);
    }

    *src = argv[optind];      /* e.g. "file1.txt" */
    *dst = argv[optind + 1];  /* e.g. "file2.txt" */
}


/* ============================================================
 * is_all_zeros — helper for Milestone 4.
 *
 * Scans 'n' bytes in 'buf' and returns 1 if every byte is zero,
 * or 0 if any non-zero byte is found.
 *
 * We use this to detect whether a buffer chunk is a "hole"
 * that we can skip writing and instead seek over in dest.
 * ============================================================ */
static int
is_all_zeros(const char *buf, ssize_t n)
{
    ssize_t i;
    for (i = 0; i < n; i++) {
        if (buf[i] != 0)
            return 0;   /* found a non-zero byte — not a hole */
    }
    return 1;           /* all zeros — this is a hole */
}

/* ============================================================
 * COPY_RAW — Milestones 1, 2 & 4: copy using open/read/write.
 *
 * Step-by-step:
 *   1. [M2] stat() source to save its permissions + block count
 *   2. Open source for reading
 *   3. Open/create destination (with O_EXCL if -n)
 *   4. Copy loop:
 *        [M4] if -S and buffer is all zeros → lseek() over it
 *             otherwise → write() the buffer normally
 *   5. [M4] ftruncate() to fix final size if file ends in a hole
 *   6. [M2] chmod() dest to match source permissions if -p
 *   7. [M2] chown() dest if -p and we are root
 *   8. Close both files
 *   9. On any error: clean up and exit
 * ============================================================ */
static void
copy_raw(const char *src, const char *dst)
{
    int     src_fd, dst_fd;      /* file descriptors (just integers) */
    char    buf[BUFFSIZE];       /* our 4096-byte buffer/bucket      */
    ssize_t n_read, n_written;   /* bytes read or written            */
    ssize_t n_left;              /* bytes still left to write        */
    char   *write_ptr;           /* pointer into buf for partial writes */
    int     dst_flags;           /* flags we'll pass to open() for dest */
    struct stat src_st;          /* stat struct to hold source metadata */

    /* ----------------------------------------------------------
     * STEP 1: stat() the source file BEFORE opening it.
     *
     * stat() fills a struct stat with file metadata:
     *   st_mode   = permission bits (e.g. rwxr-x---)
     *   st_uid    = owner user ID
     *   st_gid    = owner group ID
     *   st_size   = logical file size in bytes
     *   st_blocks = actual 512-byte blocks used on disk
     *
     * MILESTONE 4 uses st_size and st_blocks to detect sparseness:
     *   if (st_blocks * 512 < st_size) → file has holes!
     * ---------------------------------------------------------- */
    if (stat(src, &src_st) == -1)
        die(src);

    /* ----------------------------------------------------------
     * STEP 2: Open the source file for reading only.
     * ---------------------------------------------------------- */
    src_fd = open(src, O_RDONLY);
    if (src_fd == -1)
        die(src);

    /* ----------------------------------------------------------
     * STEP 3: Build flags and open the destination file.
     *
     * -n adds O_EXCL (atomic no-clobber, avoids race condition).
     * Without -n we use O_TRUNC to wipe existing content.
     *
     * WHY NOT use access(2) then open(2)?
     * That is a RACE CONDITION (TOCTOU). Between access() and
     * open(), another process could create the file. O_EXCL with
     * O_CREAT makes the check-and-create one atomic OS operation.
     * ---------------------------------------------------------- */
    dst_flags = O_WRONLY | O_CREAT;
    if (opt_noclobber)
        dst_flags |= O_EXCL;
    else
        dst_flags |= O_TRUNC;

    dst_fd = open(dst, dst_flags, 0666);
    if (dst_fd == -1) {
        close(src_fd);
        if (opt_noclobber && errno == EEXIST)
            die("destination already exists (use without -n to overwrite)");
        die(dst);
    }

    /* ----------------------------------------------------------
     * STEP 4: The copy loop — with optional sparse hole support.
     *
     * MILESTONE 4 — How sparse copying works:
     *
     *   A sparse file has "holes" — regions of zeros that consume
     *   no real disk blocks. A naive write() fills them with real
     *   zeros, wasting space. Instead we:
     *
     *   a) Check if the source likely has holes:
     *      st_blocks * 512 < st_size → holes detected
     *
     *   b) For each buffer read, check if it is all zeros.
     *      If yes → seek forward in dest instead of writing.
     *      lseek(dst_fd, n_read, SEEK_CUR) moves the write cursor
     *      forward without touching any disk blocks. When you later
     *      write past this point, the OS creates a real hole.
     *
     *   c) If the buffer has real data → write() it normally.
     *
     * lseek() with SEEK_CUR:
     *   Moves the file cursor forward by n_read bytes from its
     *   current position. Think of it as "skip ahead."
     * ---------------------------------------------------------- */

    /* Should we even bother checking for holes? */
    int src_is_sparse = opt_sparse &&
                        (src_st.st_blocks * 512 < src_st.st_size);

    while ((n_read = read(src_fd, buf, BUFFSIZE)) > 0) {

        /* ── SPARSE PATH ───────────────────────────────────────
         * If -S is set AND source appears sparse AND this entire
         * buffer chunk is all zeros → seek over it in dest.
         * No bytes written = no disk blocks used = hole preserved.
         * ------------------------------------------------------ */
        if (src_is_sparse && is_all_zeros(buf, n_read)) {
            if (lseek(dst_fd, n_read, SEEK_CUR) == -1)
                cleanup_and_die(dst, "lseek error");
            continue;   /* go back to read the next chunk */
        }

        /* ── NORMAL PATH ───────────────────────────────────────
         * Real data (or -S not set) → write everything we read.
         * Inner loop handles partial writes correctly.
         * ------------------------------------------------------ */
        n_left    = n_read;
        write_ptr = buf;

        while (n_left > 0) {
            n_written = write(dst_fd, write_ptr, n_left);
            if (n_written == -1)
                cleanup_and_die(dst, "write error");
            n_left    -= n_written;
            write_ptr += n_written;
        }
    }

    if (n_read == -1)
        cleanup_and_die(dst, "read error");

    /* ----------------------------------------------------------
     * MILESTONE 4 — STEP 5: Fix final file size with ftruncate().
     *
     * PROBLEM: What if the file ENDS in a hole?
     *
     *   Source:  [data][  HOLE  ]
     *                           ↑ st_size = here (e.g. 1,048,576)
     *
     *   After our loop, we lseek()'d past the hole but never wrote
     *   anything after it. The dest file cursor is sitting in the
     *   middle of nowhere. Its actual size on disk may be SHORTER
     *   than st_size because the OS only extends the file when you
     *   actually write to it.
     *
     * SOLUTION: ftruncate(dst_fd, src_st.st_size)
     *   This tells the OS: "make this file exactly st_size bytes."
     *   If the file is shorter → it extends it (creating a hole).
     *   If the file is already correct → it's a no-op.
     *
     * We only need this when sparse mode is active.
     * ---------------------------------------------------------- */
    if (opt_sparse) {
        if (ftruncate(dst_fd, src_st.st_size) == -1)
            cleanup_and_die(dst, "ftruncate error");
    }

    /* ----------------------------------------------------------
     * STEP 6: Preserve permissions with chmod() if -p.
     *
     * WHY does dest have fewer bits without -p?
     * The OS applies the user's UMASK when creating files.
     * umask removes bits — e.g. umask 022 strips group/other write.
     *   source : 0750 → rwxr-x---
     *   umask  : 0022 → removes write bits
     *   result : 0644 → rw-r--r--  (x bits stripped!)
     *
     * chmod() bypasses umask and sets the mode directly.
     * & 07777 keeps only the permission bits of st_mode.
     * ---------------------------------------------------------- */
    if (opt_preserve) {
        if (chmod(dst, src_st.st_mode & 07777) == -1)
            cleanup_and_die(dst, "chmod failed");

        /* ----------------------------------------------------------
         * STEP 7: Preserve ownership with chown() if root.
         *
         * Only root (euid == 0) can change file ownership.
         * Non-root silently skips — not an error.
         * ---------------------------------------------------------- */
        if (geteuid() == 0) {
            if (chown(dst, src_st.st_uid, src_st.st_gid) == -1)
                cleanup_and_die(dst, "chown failed");
        }
    }

    /* ----------------------------------------------------------
     * STEP 8: Close both files.
     * Always close — unclosed fds are resource leaks.
     * ---------------------------------------------------------- */
    if (close(src_fd) == -1)
        die("close source");

    if (close(dst_fd) == -1)
        die("close dest");
}


/* ============================================================
 * COPY_STDIO — Milestone 5: Buffered copy using stdio.
 * Will be implemented later.
 * ============================================================ */
static void
copy_stdio(const char *src, const char *dst)
{
    /* TODO Milestone 5 */
    (void)src; (void)dst;
}


/* ============================================================
 * COPY_SYMLINK — Milestone 3: Create dst as a symlink → src.
 *
 * A symbolic link is just a tiny file that stores a path string.
 * When the OS sees it, it redirects to the target automatically.
 *
 * WHY lstat() and NOT stat() here?
 *   stat()  follows symlinks — it reports on the TARGET file.
 *   lstat() stops at the link — it reports on the LINK ITSELF.
 *
 *   We use lstat() in two places:
 *     1. Before creating: to check if src exists and dst doesn't
 *     2. After creating:  to verify dst is truly a symlink
 *
 *   If we used stat() to check dst after creation, and dst pointed
 *   to a file, stat() would tell us about that file — not confirm
 *   the link itself was created. lstat() gives us ground truth.
 *
 * Step-by-step:
 *   1. lstat(src) → confirm source exists
 *   2. lstat(dst) → if dest already exists, refuse (always no-clobber)
 *   3. symlink(src, dst) → create the link
 *   4. lstat(dst) → verify the result is a symlink (S_ISLNK check)
 * ============================================================ */
static void
copy_symlink(const char *src, const char *dst)
{
    struct stat st;   /* reused for both src and dst checks */

    /* ----------------------------------------------------------
     * STEP 1: Confirm the source exists using lstat().
     *
     * We use lstat() (not stat()) so that if src is itself a
     * symlink, we still detect it correctly — stat() would follow
     * it and report on the final target, potentially misleading us.
     * If src doesn't exist at all, lstat() returns -1 with ENOENT.
     * ---------------------------------------------------------- */
    if (lstat(src, &st) == -1)
        die(src);   /* source not found or unreadable */

    /* ----------------------------------------------------------
     * STEP 2: Check if dest already exists.
     *
     * -s always behaves like -n (no-clobber).
     * We never overwrite an existing file with a symlink — that
     * could silently destroy data the user didn't mean to lose.
     *
     * lstat() returning 0 means dst EXISTS → refuse and exit.
     * lstat() returning -1 with errno == ENOENT means dst does
     * NOT exist → that's what we want, so we continue.
     * Any other error (e.g. permission denied) is a real problem.
     * ---------------------------------------------------------- */
    if (lstat(dst, &st) == 0) {
        /* dst exists — refuse */
        errno = EEXIST;
        die(dst);
    } else if (errno != ENOENT) {
        /* lstat failed for a reason OTHER than "not found" */
        die(dst);
    }
    /* If errno == ENOENT: dst doesn't exist — we're good to go */

    /* ----------------------------------------------------------
     * STEP 3: Create the symbolic link.
     *
     * symlink(oldpath, newpath) creates a symlink at newpath
     * that points to oldpath. In our case:
     *   symlink(src, dst)
     *   → creates dst as a link pointing to src
     *
     * No data from src is read or copied — the OS just stores
     * the string src inside the new link file at dst.
     * ---------------------------------------------------------- */
    if (symlink(src, dst) == -1)
        die(dst);

    /* ----------------------------------------------------------
     * STEP 4: Verify the result with lstat().
     *
     * After calling symlink(), we confirm it worked by calling
     * lstat() on dst and checking that it's truly a symlink.
     *
     * S_ISLNK(st.st_mode) is a macro that returns non-zero if
     * the file type bits indicate a symbolic link.
     *
     * We use lstat() here on purpose — if we used stat(), it would
     * follow the new link to src and tell us about src, not dst.
     * ---------------------------------------------------------- */
    if (lstat(dst, &st) == -1)
        die(dst);

    if (!S_ISLNK(st.st_mode)) {
        /* This should never happen, but be defensive */
        fprintf(stderr, "mycp: %s: created but is not a symlink!\n", dst);
        exit(1);
    }
}


/* ============================================================
 * DIE — Print an error message and exit.
 *
 * strerror(errno) converts the error number into a human-readable
 * string. For example, errno=2 → "No such file or directory"
 * ============================================================ */
static void
die(const char *msg)
{
    fprintf(stderr, "mycp: %s: %s\n", msg, strerror(errno));
    exit(1);
}


/* ============================================================
 * CLEANUP_AND_DIE — Remove the partial destination file, then die.
 *
 * If we fail halfway through copying, the destination file is
 * incomplete/corrupted. We use unlink() to delete it so we don't
 * leave behind a broken file.
 * ============================================================ */
static void
cleanup_and_die(const char *dst, const char *msg)
{
    unlink(dst);   /* delete the partial/broken destination file */
    die(msg);
}