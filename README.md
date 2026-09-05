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
