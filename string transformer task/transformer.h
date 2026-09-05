#ifndef TRANSFORMER_H
#define TRANSFORMER_H

/* Function prototypes for the transformer program */

/**
 * Reads a single command‑line argument into a newly allocated buffer.
 * All safety checks are performed here.
 *
 * @param argc  Number of arguments (from main)
 * @param argv  Argument vector (from main)
 * @return      Pointer to a buffer containing the argument string.
 *              The caller is responsible for freeing it.
 */
char *read_argument(int argc, char **argv);

/**
 * Splits a multi‑line string into an array of lines.
 * The original buffer is modified in place: each '\n' is replaced by '\0'.
 * The returned array is terminated by a NULL pointer.
 *
 * @param buffer  The multi‑line string (must be writable)
 * @return        Pointer to a NULL‑terminated array of line pointers.
 *                The caller must free this array when done.
 */
char **split_lines(char *buffer);

/**
 * Splits a line into words (separated by one or more spaces).
 * The original string is modified in place: each space is replaced by '\0'.
 * The returned array is terminated by a NULL pointer.
 *
 * @param line  The input line (must be writable, null‑terminated).
 * @return      Pointer to a NULL‑terminated array of word pointers.
 *              The caller must free this array when done.
 */
char **split_words(char *line);

/**
 * Splits a multi‑line buffer into an array of line‑based word arrays.
 * It first uses split_lines to get lines, then split_words for each line.
 * The original buffer is modified in place (newlines and spaces become '\0').
 * Returns a NULL‑terminated array of word arrays (each word array is also NULL‑terminated).
 */
char ***split_into_word_arrays(char *buffer);

/**
 * Reverses the order of words in a NULL‑terminated array of word pointers.
 * The array is modified in place; no new memory is allocated.
 *
 * @param words  Pointer to a NULL‑terminated array of char* pointers.
 *               Must not be NULL.
 */
void reverse_word_order(char **words);

/**
 * Reverses the order of characters in a word.
 * The word is assumed to start at the given pointer and continues until
 * either a space or the null terminator is encountered.
 * The terminating character (space or '\0') is left unchanged.
 *
 * @param word  Pointer to the first character of the word.
 *              Must not be NULL.
 */
void reverse_word(char *word);

/**
 * Prints the content of a NULL‑terminated array of word arrays.
 * Each word array is NULL‑terminated.
 * Lines are printed in order; words within each line are separated by spaces,
 * and each line ends with a newline.
 *
 * @param arrays  The top‑level array (char***), NULL‑terminated.
 *                Must not be NULL.
 */
void print_word_arrays(char ***arrays);

#endif /* TRANSFORMER_H */