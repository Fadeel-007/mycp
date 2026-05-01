/*
 * mycp.c — a simplified file copy utility
 *
 * Course: Advanced Programming in the UNIX Environment
 * Covers: APUE Chapters 3, 4, and 5
 *
 * Usage: mycp [-pnsSbv] source dest
 *
 *   -p   preserve permissions (and owner if root)
 *   -n   no-clobber: refuse to overwrite an existing destination
 *   -s   create a symbolic link instead of copying data
 *   -S   sparse-aware copy (preserve holes in sparse files)
 *   -b   buffered copy using stdio (fopen/fread/fwrite) instead of raw syscalls
 *   -v   verbose: print a status line on success
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFSIZE 4096

/* --------- option flags (set by parse_args) ------------------------------ */
static int opt_preserve = 0;   /* -p */
static int opt_noclobber = 0;  /* -n */
static int opt_symlink = 0;    /* -s */
static int opt_sparse = 0;     /* -S */
static int opt_buffered = 0;   /* -b */
static int opt_verbose = 0;    /* -v */

/* --------- helpers ------------------------------------------------------- */

/* Print "mycp: <ctx>: <strerror(errno)>" and exit(1). */
static void
die(const char *ctx)
{
    fprintf(stderr, "mycp: %s: %s\n", ctx, strerror(errno));
    exit(1);
}

/* Same as die(), but first try to remove the half-written destination. */
static void
cleanup_and_die(const char *dst, const char *ctx)
{
    int saved = errno;       /* unlink() may overwrite errno; save it first */
    unlink(dst);
    errno = saved;
    die(ctx);
}

/* Return 1 if every byte in buf[0..n-1] is zero, else 0. */
static int
is_all_zeros(const char *buf, ssize_t n)
{
    ssize_t i;
    for (i = 0; i < n; i++)
        if (buf[i] != 0)
            return 0;
    return 1;
}

/* Print the verbose status line. Must go to stderr per the assignment. */
static void
print_verbose(const char *src, const char *dst, off_t bytes)
{
    if (opt_symlink)
        fprintf(stderr, "mycp: '%s' -> '%s'  (symlink)\n", src, dst);
    else
        fprintf(stderr, "mycp: '%s' -> '%s'  (%lld bytes copied)\n",
                src, dst, (long long)bytes);
}

/* --------- argument parsing --------------------------------------------- */

static void
usage(void)
{
    fprintf(stderr, "usage: mycp [-pnsSbv] source dest\n");
    exit(1);
}

static void
parse_args(int argc, char *argv[], const char **src, const char **dst)
{
    int c;
    while ((c = getopt(argc, argv, "pnsSbv")) != -1) {
        switch (c) {
        case 'p': opt_preserve = 1;  break;
        case 'n': opt_noclobber = 1; break;
        case 's': opt_symlink = 1;   break;
        case 'S': opt_sparse = 1;    break;
        case 'b': opt_buffered = 1;  break;
        case 'v': opt_verbose = 1;   break;
        default:  usage();
        }
    }
    if (argc - optind != 2)
        usage();
    *src = argv[optind];
    *dst = argv[optind + 1];
}

/* --------- Milestone 3: symlink ----------------------------------------- */

static void
copy_symlink(const char *src, const char *dst)
{
    struct stat st;

    /* 1. Make sure source exists (use lstat so we don't follow it). */
    if (lstat(src, &st) < 0)
        die(src);

    /* 2. Refuse if dst already exists (always no-clobber for symlinks). */
    if (lstat(dst, &st) == 0) {
        errno = EEXIST;
        die(dst);
    } else if (errno != ENOENT) {
        /* lstat failed for some reason other than "not found" */
        die(dst);
    }

    /* 3. Create the symlink. */
    if (symlink(src, dst) < 0)
        die(dst);

    /* 4. Verify it really is a symlink. */
    if (lstat(dst, &st) < 0 || !S_ISLNK(st.st_mode)) {
        errno = EIO;
        die(dst);
    }

    if (opt_verbose)
        print_verbose(src, dst, 0);
}

/* --------- Milestone 1, 2, 4: raw copy ---------------------------------- */

static void
copy_raw(const char *src, const char *dst)
{
    int         src_fd, dst_fd;
    int         dst_flags;
    ssize_t     n_read, n_written, total_written, off;
    char        buf[BUFFSIZE];
    struct stat src_st;
    off_t       bytes_total = 0;
    int         src_is_sparse;

    /* 1. Open the source file for reading. */
    if ((src_fd = open(src, O_RDONLY)) < 0)
        die(src);

    /* 2. Stat the source to learn its size, permissions, and block count. */
    if (fstat(src_fd, &src_st) < 0) {
        close(src_fd);
        die(src);
    }

    /* Decide whether the source is actually sparse:
     *   logical size  = src_st.st_size
     *   real disk use = src_st.st_blocks * 512
     * If the real disk use is less than the logical size, there are holes. */
    src_is_sparse = opt_sparse &&
                    ((off_t)src_st.st_blocks * 512 < src_st.st_size);

    /* 3. Build dest open flags.
     *    -n -> O_EXCL: atomic "create only if not exists"
     *    otherwise -> O_TRUNC: overwrite */
    dst_flags = O_WRONLY | O_CREAT;
    dst_flags |= opt_noclobber ? O_EXCL : O_TRUNC;

    if ((dst_fd = open(dst, dst_flags, 0666)) < 0) {
        close(src_fd);
        die(dst);
    }

    /* 4. The copy loop.
     *    On every chunk, decide: is this a hole-shaped chunk we should skip,
     *    or is it real data we must write? */
    while ((n_read = read(src_fd, buf, BUFFSIZE)) > 0) {

        if (src_is_sparse && is_all_zeros(buf, n_read)) {
            /* Skip forward in dest instead of writing zeros.
             * This recreates the hole on the destination. */
            if (lseek(dst_fd, n_read, SEEK_CUR) < 0) {
                close(src_fd); close(dst_fd);
                cleanup_and_die(dst, dst);
            }
        } else {
            /* Real data: write it, looping in case of partial writes. */
            total_written = 0;
            while (total_written < n_read) {
                off = total_written;
                n_written = write(dst_fd, buf + off, n_read - off);
                if (n_written < 0) {
                    if (errno == EINTR) continue;
                    close(src_fd); close(dst_fd);
                    cleanup_and_die(dst, dst);
                }
                total_written += n_written;
            }
        }
        bytes_total += n_read;
    }

    if (n_read < 0) {
        close(src_fd); close(dst_fd);
        cleanup_and_die(dst, src);
    }

    /* 5. If the source ends in a hole, our last action was lseek() — the
     *    file on disk is shorter than st_size. Force the correct length. */
    if (src_is_sparse) {
        if (ftruncate(dst_fd, src_st.st_size) < 0) {
            close(src_fd); close(dst_fd);
            cleanup_and_die(dst, dst);
        }
    }

    /* 6. Preserve permissions (and owner if root) when -p was given. */
    if (opt_preserve) {
        if (fchmod(dst_fd, src_st.st_mode & 07777) < 0) {
            close(src_fd); close(dst_fd);
            cleanup_and_die(dst, dst);
        }
        if (geteuid() == 0) {
            if (fchown(dst_fd, src_st.st_uid, src_st.st_gid) < 0) {
                close(src_fd); close(dst_fd);
                cleanup_and_die(dst, dst);
            }
        }
    }

    /* 7. Close both files. */
    if (close(src_fd) < 0)
        die(src);
    if (close(dst_fd) < 0)
        cleanup_and_die(dst, dst);

    if (opt_verbose)
        print_verbose(src, dst, bytes_total);
}

/* --------- Milestone 5: stdio (buffered) copy --------------------------- */

static void
copy_stdio(const char *src, const char *dst)
{
    FILE       *src_fp, *dst_fp;
    int         src_fd, dst_fd;
    int         dst_flags;
    size_t      n_read, n_written;
    char        buf[BUFFSIZE];
    char        my_buf[BUFFSIZE];   /* our own buffer for setvbuf on dest */
    struct stat src_st;
    off_t       bytes_total = 0;
    const char *open_mode;

    /* 1. Open source with fopen() in read-binary mode. */
    if ((src_fp = fopen(src, "rb")) == NULL)
        die(src);

    /* 2. Bridge to syscall world to fstat() the source. */
    src_fd = fileno(src_fp);
    if (fstat(src_fd, &src_st) < 0) {
        fclose(src_fp);
        die(src);
    }

    /* 3. Open dest. fopen() doesn't have an O_EXCL equivalent directly,
     *    so we use open() with the right flags, then fdopen() to wrap it. */
    dst_flags = O_WRONLY | O_CREAT;
    dst_flags |= opt_noclobber ? O_EXCL : O_TRUNC;

    if ((dst_fd = open(dst, dst_flags, 0666)) < 0) {
        fclose(src_fp);
        die(dst);
    }

    open_mode = "wb";
    if ((dst_fp = fdopen(dst_fd, open_mode)) == NULL) {
        close(dst_fd);
        fclose(src_fp);
        die(dst);
    }

    /* 4. Set fully-buffered mode on the destination, using OUR buffer.
     *    This is the educational point of -b: we control the buffering. */
    if (setvbuf(dst_fp, my_buf, _IOFBF, BUFFSIZE) != 0) {
        fclose(src_fp); fclose(dst_fp);
        cleanup_and_die(dst, dst);
    }

    /* 5. Copy loop using fread/fwrite.
     *    fread returns the number of items (each of size 1) actually read. */
    while ((n_read = fread(buf, 1, BUFFSIZE, src_fp)) > 0) {
        n_written = fwrite(buf, 1, n_read, dst_fp);
        if (n_written != n_read) {
            fclose(src_fp); fclose(dst_fp);
            cleanup_and_die(dst, dst);
        }
        bytes_total += (off_t)n_read;
    }

    /* 6. fread returns 0 both on EOF and on error — distinguish them. */
    if (ferror(src_fp)) {
        fclose(src_fp); fclose(dst_fp);
        cleanup_and_die(dst, src);
    }

    /* 7. Preserve permissions before closing (we still have the fd). */
    if (opt_preserve) {
        if (fchmod(dst_fd, src_st.st_mode & 07777) < 0) {
            fclose(src_fp); fclose(dst_fp);
            cleanup_and_die(dst, dst);
        }
        if (geteuid() == 0) {
            if (fchown(dst_fd, src_st.st_uid, src_st.st_gid) < 0) {
                fclose(src_fp); fclose(dst_fp);
                cleanup_and_die(dst, dst);
            }
        }
    }

    /* 8. Close both streams. fclose flushes the buffer first. */
    if (fclose(src_fp) != 0)
        die(src);
    if (fclose(dst_fp) != 0)
        cleanup_and_die(dst, dst);

    if (opt_verbose)
        print_verbose(src, dst, bytes_total);
}

/* --------- main --------------------------------------------------------- */

int
main(int argc, char *argv[])
{
    const char *src, *dst;

    /* Make stderr line-buffered so verbose/error messages appear promptly. */
    setvbuf(stderr, NULL, _IOLBF, 0);

    parse_args(argc, argv, &src, &dst);

    /* Dispatch: symlink mode wins if -s, otherwise raw or buffered copy. */
    if (opt_symlink)
        copy_symlink(src, dst);
    else if (opt_buffered)
        copy_stdio(src, dst);
    else
        copy_raw(src, dst);

    return 0;
}