/*
 * msgbus.cpp - a simple file-based message bus.
 *
 * Identity model: each invocation of this program represents one
 * terminal.  The terminal's ID is passed as the first argument and
 * is fixed for the lifetime of the process.  A terminal can only
 * send messages "from" its own ID, and can only read messages whose
 * receiver_id equals its own ID.  This is enforced in main() and in
 * the command handlers; nothing on the command line overrides it.
 *
 * File layout (all offsets are bytes from start of file):
 *
 *   [ Header         ]   64 bytes   offset 0
 *   [ Descriptor[0]  ]   48 bytes   offset 64
 *   [ Descriptor[1]  ]   48 bytes   offset 112
 *   ...
 *   [ Data area      ]   variable   offset header.data_start
 *
 * Every field is a fixed width.  Nothing in the file is a memory
 * pointer -- descriptors store file offsets, not addresses.
 *
 * Concurrency:  every read-modify-write is guarded by fcntl(F_SETLKW)
 * on the whole file.
 *
 * Integrity: every header and descriptor carries an FNV-1a checksum.
 * On a mismatch, a descriptor is skipped (read path) or the program
 * aborts before writing (write path), so corrupted data never
 * propagates.
 *
 * Build:  gcc -std=c++17 -Wall -Wextra -O2 -o msgbus msgbus.cpp
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>

#include "msgbus.h"

static_assert(sizeof(Header)     == 64, "Header must be 64 bytes");
static_assert(sizeof(Descriptor) == 48, "Descriptor must be 48 bytes");

/* --------------------------- Error handling ---------------------------- */

/*
 * Print an error message together with the source location.
 * Used both for recoverable warnings and as part of fail().
 */
void print_error(const char *where, const char *msg)
{
    std::fprintf(stderr, "[msgbus] %s: %s\n", where, msg);
}

/*
 * Unrecoverable error. Prints the location and message, then exits.
 * Called only from paths where continuing could write inconsistent
 * data to the file (bad header, IO error, allocation failure during
 * a write).
 */
void fail(const char *where, const char *msg)
{
    print_error(where, msg);
    std::exit(EXIT_FAILURE);
}

/* ----------------------------- Checksums ------------------------------- */

/*
 * FNV-1a 32-bit hash. Small, fast, no tables, good enough to detect
 * accidental corruption (torn writes, bit flips, mixed-up versions).
 */
static uint32_t fnv1a(const void *buf, size_t n)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t header_checksum(const Header *h)
{
    return fnv1a(h, offsetof(Header, checksum));
}

static uint32_t descriptor_checksum(const Descriptor *d)
{
    return fnv1a(d, offsetof(Descriptor, checksum));
}

/* -------------------------- Raw IO wrappers ---------------------------- */

/*
 * Loop until we've read/written exactly n bytes at the given offset.
 * pread and pwrite can perform short transfers; ignoring that is a
 * classic bug in file code.
 */
static void read_exact(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0)  fail("read_exact", std::strerror(errno));
        if (r == 0) fail("read_exact", "unexpected end of file");
        p += r;
        n -= static_cast<size_t>(r);
        off += r;
    }
}

static void write_exact(int fd, const void *buf, size_t n, off_t off)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w < 0) fail("write_exact", std::strerror(errno));
        p += w;
        n -= static_cast<size_t>(w);
        off += w;
    }
}

/* ------------------------------ Locking -------------------------------- */

static void lock_file(int fd, short type)
{
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;                    /* whole file */
    if (fcntl(fd, F_SETLKW, &fl) == -1)
        fail("lock_file", std::strerror(errno));
}

static void unlock_file(int fd)
{
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    if (fcntl(fd, F_SETLK, &fl) == -1)
        fail("unlock_file", std::strerror(errno));
}

/* --------------------- Header and descriptor IO ------------------------ */

static void read_header(int fd, Header *h)
{
    read_exact(fd, h, sizeof(*h), 0);
    if (h->magic != MAGIC)
        fail("read_header", "bad magic (not a msgbus file?)");
    if (h->version != VERSION)
        fail("read_header", "unsupported file version");
    if (header_checksum(h) != h->checksum)
        fail("read_header", "header checksum mismatch (file corrupted)");
}

static void write_header(int fd, Header *h)
{
    h->checksum = header_checksum(h);
    write_exact(fd, h, sizeof(*h), 0);
}

/*
 * Returns 1 on success, 0 if the descriptor's checksum is wrong.
 * A checksum mismatch means the slot is unusable: the caller must
 * skip it. We never hand back a descriptor we cannot trust.
 */
static int read_descriptor(int fd, uint32_t slot, Descriptor *d)
{
    off_t off = sizeof(Header) + static_cast<off_t>(slot) * sizeof(Descriptor);
    read_exact(fd, d, sizeof(*d), off);
    if (descriptor_checksum(d) != d->checksum) {
        print_error("read_descriptor", "checksum mismatch, skipping slot");
        return 0;
    }
    return 1;
}

static void write_descriptor(int fd, uint32_t slot, Descriptor *d)
{
    off_t off = sizeof(Header) + static_cast<off_t>(slot) * sizeof(Descriptor);
    d->checksum = descriptor_checksum(d);
    write_exact(fd, d, sizeof(*d), off);
}

/* -------------------------- File lifecycle ----------------------------- */

/*
 * Create a fresh, empty bus. Called once before any other command.
 */
void init_file(void)
{
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        fail("init_file", std::strerror(errno));

    Header h;
    std::memset(&h, 0, sizeof(h));
    h.magic         = MAGIC;
    h.version       = VERSION;
    h.max_messages  = MAX_MESSAGES;
    h.message_count = 0;
    h.next_guid     = 1;
    h.data_start    = sizeof(Header) +
                      static_cast<uint64_t>(MAX_MESSAGES) * sizeof(Descriptor);
    h.data_end      = h.data_start;
    h.generation    = 1;
    write_header(fd, &h);

    /* Pre-size the descriptor array so the file has a stable layout. */
    Descriptor zero;
    std::memset(&zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < MAX_MESSAGES; i++) {
        write_exact(fd, &zero, sizeof(zero),
                    sizeof(Header) +
                    static_cast<off_t>(i) * sizeof(Descriptor));
    }

    close(fd);
}

/* --------------------------- Command: send ----------------------------- */

/*
 * Send a message.  The transmitter is always 'terminal_id' -- the
 * caller cannot supply a different sender.
 */
void cmd_send(int fd, uint32_t terminal_id, uint32_t receiver,
                     const char *msg)
{
    size_t msg_size = std::strlen(msg);
    if (msg_size == 0 || msg_size > MAX_MSG_SIZE) {
        fail("cmd_send", "message size out of range");
    }

    lock_file(fd, F_WRLCK);

    Header h;
    read_header(fd, &h);

    if (h.message_count >= h.max_messages) {
        unlock_file(fd);
        fail("cmd_send", "message bus is full");
    }

    uint32_t slot = h.message_count;

    /*
     * Write order matters:
     *   1. payload   (invisible until a descriptor points at it)
     *   2. descriptor (invisible until message_count includes it)
     *   3. header    (publishes the new slot atomically)
     * If we crash at any point, a reader never sees a half-written
     * message.
     */
    uint64_t payload_off = h.data_end;
    write_exact(fd, msg, msg_size, static_cast<off_t>(payload_off));

    Descriptor d;
    std::memset(&d, 0, sizeof(d));
    d.guid            = h.next_guid++;
    d.appearance_time = static_cast<uint64_t>(std::time(NULL));
    d.msg_offset      = payload_off;
    d.transmitter_id  = terminal_id;    /* always this terminal */
    d.receiver_id     = receiver;
    d.msg_size        = static_cast<uint32_t>(msg_size);
    d.is_processed    = 0;
    write_descriptor(fd, slot, &d);

    h.message_count++;
    h.data_end += msg_size;
    write_header(fd, &h);

    std::printf("terminal %u sent guid=%llu to terminal %u\n",
                terminal_id,
                static_cast<unsigned long long>(d.guid),
                receiver);

    unlock_file(fd);
}

/* --------------------------- Command: read ----------------------------- */

/*
 * Read all unread messages whose receiver_id == terminal_id.
 * Mark each one as processed.  No message for any other terminal
 * is ever printed.
 */
void cmd_read(int fd, uint32_t terminal_id)
{
    /*
     * A write lock is used because after reading each message
     * we mark it as processed, which is a file write.
     */
    lock_file(fd, F_WRLCK);

    Header h;
    read_header(fd, &h);

    std::printf("terminal %u: messages for this terminal:\n", terminal_id);

    int found = 0;
    for (uint32_t slot = 0; slot < h.message_count; slot++) {
        Descriptor d;
        if (!read_descriptor(fd, slot, &d))
            continue;                   /* checksum failed, skip */
        if (d.is_processed != 0) continue;
        if (d.receiver_id != terminal_id) continue;

        char *buf = static_cast<char *>(std::malloc(d.msg_size + 1));
        if (!buf) {
            unlock_file(fd);
            fail("cmd_read", "out of memory");
        }
        read_exact(fd, buf, d.msg_size, static_cast<off_t>(d.msg_offset));
        buf[d.msg_size] = '\0';

        std::printf("  [guid=%llu from terminal %u] %s\n",
                    static_cast<unsigned long long>(d.guid),
                    d.transmitter_id, buf);
        std::free(buf);

        d.is_processed = 1;
        write_descriptor(fd, slot, &d);
        found++;
    }

    if (found == 0)
        std::printf("  (no new messages)\n");

    unlock_file(fd);
}

/* --------------------------- Command: clean ---------------------------- */

void cmd_clean(int fd)
{
    lock_file(fd, F_WRLCK);

    Header h;
    read_header(fd, &h);

    uint64_t now = static_cast<uint64_t>(std::time(NULL));

    /* Allocate room for the compacted data area. */
    size_t cap = static_cast<size_t>(h.data_end - h.data_start);
    if (cap == 0) cap = 1;
    char *new_data = static_cast<char *>(std::malloc(cap));
    if (!new_data) {
        unlock_file(fd);
        fail("cmd_clean", "out of memory");
    }

    /* Build the new descriptor list in memory first. */
    Descriptor *new_desc =
        static_cast<Descriptor *>(std::calloc(h.max_messages, sizeof(Descriptor)));
    if (!new_desc) {
        std::free(new_data);
        unlock_file(fd);
        fail("cmd_clean", "out of memory");
    }
    uint32_t new_count = 0;
    size_t   new_size  = 0;

    for (uint32_t slot = 0; slot < h.message_count; slot++) {
        Descriptor d;
        if (!read_descriptor(fd, slot, &d))
            continue;

        int is_old = (d.appearance_time + TTL_SECONDS < now);
        if (d.is_processed == 1 || is_old)
            continue;                   /* drop */

        /* Keep it: copy payload into the compacted buffer. */
        char *buf = static_cast<char *>(std::malloc(d.msg_size));
        if (!buf) {
            std::free(new_data);
            std::free(new_desc);
            unlock_file(fd);
            fail("cmd_clean", "out of memory");
        }
        read_exact(fd, buf, d.msg_size, static_cast<off_t>(d.msg_offset));
        std::memcpy(new_data + new_size, buf, d.msg_size);
        std::free(buf);

        d.msg_offset = h.data_start + new_size;
        new_desc[new_count++] = d;
        new_size += d.msg_size;
    }

    /* Rewrite the data area at data_start. */
    if (new_size > 0)
        write_exact(fd, new_data, new_size, static_cast<off_t>(h.data_start));

    /* Rewrite the descriptors we kept, in their new slots. */
    for (uint32_t i = 0; i < new_count; i++)
        write_descriptor(fd, i, &new_desc[i]);

    /* Update the header last. */
    h.message_count = new_count;
    h.data_end      = h.data_start + new_size;
    h.generation++;
    write_header(fd, &h);

    /* Trim the file so the freed tail is actually gone. */
    if (ftruncate(fd, static_cast<off_t>(h.data_end)) == -1)
        print_error("cmd_clean", std::strerror(errno));

    std::free(new_data);
    std::free(new_desc);

    std::printf("clean done: %u message(s) kept\n", new_count);

    unlock_file(fd);
}

/* -------------------------------- main --------------------------------- */

int open_bus(void)
{
    int fd = open(FILE_PATH, O_RDWR);
    if (fd < 0)
        fail("open_bus", std::strerror(errno));
    return fd;
}

/*
 * Parse the terminal id.  This is the identity of the terminal for
 * the entire process; it cannot be changed after this call.
 */
uint32_t parse_terminal_id(const char *s)
{
    char *end;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 1000000)
        fail("parse_terminal_id", "invalid terminal id");
    return static_cast<uint32_t>(v);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        std::fprintf(stderr,
            "usage:\n"
            "  %s <terminal_id> init\n"
            "  %s <terminal_id> send <receiver_id> <message>\n"
            "  %s <terminal_id> read\n"
            "  %s <terminal_id> clean\n",
            argv[0], argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * The first argument is this terminal's identity.  Every command
     * acts on behalf of this terminal and only this terminal.
     */
    uint32_t    terminal_id = parse_terminal_id(argv[1]);
    const char *cmd         = argv[2];

    if (std::strcmp(cmd, "init") == 0) {
        init_file();
        std::printf("bus initialized (created by terminal %u)\n", terminal_id);
        return EXIT_SUCCESS;
    }

    int fd = open_bus();

    if (std::strcmp(cmd, "send") == 0) {
        if (argc != 5)
            fail("main", "send requires <receiver_id> <message>");
        uint32_t receiver = parse_terminal_id(argv[3]);
        /* Sender is always this terminal; there is no way to override it. */
        cmd_send(fd, terminal_id, receiver, argv[4]);
    } else if (std::strcmp(cmd, "read") == 0) {
        cmd_read(fd, terminal_id);
    } else if (std::strcmp(cmd, "clean") == 0) {
        cmd_clean(fd);
    } else {
        close(fd);
        fail("main", "unknown command");
    }

    close(fd);
    return EXIT_SUCCESS;
}