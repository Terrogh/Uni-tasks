/*
 * msgbus.h - public interface for the file-based message bus.
 *
 * The implementation lives in msgbus.cpp.  This header is the
 * contract between the program's entry point (main) and the message
 * bus logic.  It also documents the on-disk format, because the
 * format is a stable interface: any tool that reads the file must
 * agree with these constants and layouts.
 */

#ifndef MSGBUS_H
#define MSGBUS_H

#include <cstdint>
#include <cstddef>

/* ---------------------------------------------------------------------- */
/*  Constants                                                              */
/*                                                                         */
/*  These define the shape and limits of the bus.  Changing them here      */
/*  changes the file format.  Any external tool that reads the file        */
/*  must be updated too.                                                   */
/* ---------------------------------------------------------------------- */

/* Default path of the bus file, relative to the working directory. */
#define FILE_PATH       "msgbus.db"

/* How many descriptors the descriptor array can hold.  The bus is
 * considered "full" once message_count reaches this value. */
#define MAX_MESSAGES    10

/* Largest payload accepted by cmd_send.  A message of exactly this
 * size is accepted; one byte larger is rejected. */
#define MAX_MSG_SIZE    100

/* Messages older than this (in seconds) are dropped by cmd_clean,
 * regardless of whether they have been read. */
#define TTL_SECONDS     1

/* Magic value stored in the header.  Used to detect files that
 * belong to a different program or were never initialised. */
#define MAGIC           0x4D534742u     /* "MSGB" */

/* Format version.  Bump when the layout changes in an
 * incompatible way.  The reader refuses any other version. */
#define VERSION         1u

/* ---------------------------------------------------------------------- */
/*  On-disk structures                                                     */
/*                                                                         */
/*  Both structs are fixed-size and packed by construction (all fields    */
/*  land on their natural alignment with no compiler padding).  The       */
/*  static_asserts in msgbus.cpp pin the sizes; if you add a field,       */
/*  re-check them.                                                         */
/*                                                                         */
/*  Nothing stored in the file is a memory pointer.  Offsets are byte     */
/*  offsets from the start of the file.                                    */
/* ---------------------------------------------------------------------- */

/*
 * Header - the first 64 bytes of the file.
 *
 * Followed immediately by MAX_MESSAGES descriptors, then the data area.
 */
typedef struct {
    uint32_t magic;             /* must equal MAGIC                      */
    uint32_t version;           /* must equal VERSION                    */
    uint32_t max_messages;      /* capacity of the descriptor array      */
    uint32_t message_count;     /* descriptors currently in use          */
    uint64_t next_guid;         /* counter for the next message's guid   */
    uint64_t data_start;        /* offset where the data area begins     */
    uint64_t data_end;          /* offset where free payload space begins*/
    uint64_t free_head;         /* reserved for future slot reuse        */
    uint64_t generation;        /* incremented on every compaction       */
    uint32_t checksum;          /* FNV-1a over the first 56 bytes        */
    uint32_t reserved;          /* padding, always zero                  */
} Header;

/*
 * Descriptor - one per message slot, 48 bytes each.
 *
 * slot i lives at offset sizeof(Header) + i * sizeof(Descriptor).
 * Descriptors with is_processed == 0 and receiver_id == my_id are the
 * messages I should read.
 */
typedef struct {
    uint64_t guid;              /* unique message id                      */
    uint64_t appearance_time;   /* Unix time when written                 */
    uint64_t msg_offset;        /* file offset of the payload             */
    uint32_t transmitter_id;    /* sender's terminal id                   */
    uint32_t receiver_id;       /* recipient's terminal id                */
    uint32_t msg_size;          /* payload length in bytes                */
    uint32_t is_processed;      /* 0 = new, 1 = read                      */
    uint32_t flags;             /* reserved for future use                */
    uint32_t checksum;          /* FNV-1a over the first 44 bytes         */
} Descriptor;

/* ---------------------------------------------------------------------- */
/*  Public API                                                             */
/* ---------------------------------------------------------------------- */

/*
 * print_error - report a recoverable problem.
 *
 * Prints "[msgbus] <where>: <msg>" to stderr.  Does not stop the
 * program: used for conditions the caller can recover from, such as
 * a single corrupted descriptor that can be skipped.
 *
 * @param where  short identifier of the calling function
 * @param msg    human-readable description of the problem
 */
void print_error(const char *where, const char *msg);

/*
 * fail - report an unrecoverable problem and terminate.
 *
 * Prints the same line as print_error, then calls exit(EXIT_FAILURE).
 * Used only on paths where continuing could write inconsistent data
 * to the file: bad header, IO failure, out-of-memory during a write.
 * Never called from a path that could still produce a correct file.
 *
 * @param where  short identifier of the calling function
 * @param msg    human-readable description of the problem
 */
void fail(const char *where, const char *msg);

/*
 * init_file - create a fresh, empty bus file.
 *
 * Opens FILE_PATH with O_RDWR | O_CREAT | O_TRUNC, writes a valid
 * header, and pre-sizes the descriptor array with zeroed entries so
 * the file has a stable layout from the start.  The file is created
 * with mode 0644.
 *
 * The caller is expected to run this exactly once, before any other
 * command.  Running it again truncates the bus.
 *
 * On any error, calls fail() and does not return.
 */
void init_file(void);

/*
 * open_bus - open the existing bus for reading and writing.
 *
 * Opens FILE_PATH with O_RDWR.  Does not create the file: a missing
 * file is an error.
 *
 * @return  a valid file descriptor, or exits via fail() on error
 */
int open_bus(void);

/*
 * cmd_send - send one message.
 *
 * Locks the file for writing, appends the payload to the data area,
 * fills the next free descriptor, and updates the header.  The
 * message's transmitter is always 'terminal_id' -- the caller cannot
 * supply a different sender.
 *
 * Ordering inside the file is payload, then descriptor, then header.
 * If the process dies at any point, a reader sees either the old
 * state or the complete new message, never a half-written one.
 *
 * Enforces size limits: msg must be non-empty and at most
 * MAX_MSG_SIZE bytes.  If the bus is full (message_count ==
 * max_messages), the send is refused.
 *
 * @param fd          open bus file descriptor
 * @param terminal_id this terminal's id (the sender)
 * @param receiver    recipient's terminal id
 * @param msg         NUL-terminated payload
 */
void cmd_send(int fd, uint32_t terminal_id, uint32_t receiver,
              const char *msg);

/*
 * cmd_read - read and mark every unread message addressed to me.
 *
 * Takes a write lock (the read path modifies the file by setting
 * is_processed = 1 on each delivered descriptor).  Walks the
 * descriptor array, prints every entry whose receiver_id equals
 * terminal_id and whose is_processed is 0, then marks each one as
 * processed.
 *
 * Descriptors whose checksum does not match are skipped: a single
 * corrupted slot does not stop the reader.
 *
 * @param fd          open bus file descriptor
 * @param terminal_id this terminal's id (the reader)
 */
void cmd_read(int fd, uint32_t terminal_id);

/*
 * cmd_clean - compact the bus, dropping processed and expired messages.
 *
 * Takes a write lock.  Builds the surviving descriptor list and the
 * compacted data area in memory, writes them back, updates the header
 * last, and finally truncates the file to the new data_end.
 *
 * A message survives if it has not been read AND its appearance_time
 * is within TTL_SECONDS of now.  Everything else is dropped.
 *
 * @param fd  open bus file descriptor
 */
void cmd_clean(int fd);

/*
 * parse_terminal_id - convert an argv string to a terminal id.
 *
 * Accepts a decimal integer in [0, 1000000].  Exits via fail() on
 * anything else: non-numeric input, trailing characters, overflow,
 * or a value outside the range.
 *
 * @param s  string from argv
 * @return   the parsed terminal id
 */
uint32_t parse_terminal_id(const char *s);

#endif /* MSGBUS_H */