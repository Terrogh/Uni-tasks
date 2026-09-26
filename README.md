# First Task

A command‑line tool that processes multi‑line text:

1. Takes **two arguments**:
   - An integer index (0‑based).
   - A multi‑line string (newlines must be quoted).
2. For each line:
   - If the word at the given index exists – reverse its letters.
   - Then reverse the order of **all** words in that line.
3. Prints the transformed lines in their original order.

---

## Build

```bash
gcc -o transformer transformer.c
```

## Example

```bash
./transformer 1 "hello world
one two three
a b c d"
dlrow hello
three owt one
d c b a
```

---

# Second Task

A file‑based message bus written in C++. Multiple terminals exchange messages through a single binary file. Each terminal is a separate process, identified by its terminal ID (passed as the first argument), and can only send as itself and read its own messages.

Built around a fixed‑size binary layout, `fcntl` file locking, and per‑record FNV‑1a checksums so that concurrent access and partial writes cannot silently corrupt the bus.

---

## Build

```bash
gcc -std=c++17 -Wall -Wextra -O2 -o msgbus msgbus.cpp
```

## Usage

```bash
./msgbus <terminal_id> init
./msgbus <terminal_id> send <receiver_id> "<message>"
./msgbus <terminal_id> read
./msgbus <terminal_id> clean
```

## Example

```bash
./msgbus 1 init
./msgbus 1 send 2 "hello from terminal 1"
./msgbus 2 read
# terminal 2: messages for this terminal:
#   [guid=1 from terminal 1] hello from terminal 1
./msgbus 1 clean
# clean done: 0 message(s) kept
```
