#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

#include "transformer.h"

/* -------------------- Functions -------------------- */
char *read_argument(int argc, char **argv, int *word_index) {
    assert(argc == 3);   /* program name, index, and string */

    /* Parse the index as a decimal integer */
    char *endptr;
    errno = 0;
    long val = strtol(argv[1], &endptr, 10);
    assert(errno == 0 && endptr != argv[1] && *endptr == '\0');
    /* Ensure it fits in an int (optional but safe) */
    assert(val >= INT_MIN && val <= INT_MAX);
    *word_index = (int)val;

    /* Read the multi‑line string from argv[2] */
    size_t len = strlen(argv[2]);
    char *buffer = (char *)calloc(len + 1, sizeof(char));
    assert(buffer != NULL);
    strcpy(buffer, argv[2]);

    return buffer;
}

char **split_lines(char *buffer) {
    assert(buffer);

    /* Count the number of newline characters */
    size_t newline_count = 0;
    for (size_t i = 0; buffer[i] != '\0'; i++) {
        if (buffer[i] == '\n')
            newline_count++;
    }

    /* Number of lines = newline_count + 1 (last line may be empty) */
    size_t num_lines = newline_count + 1;

    /* Allocate array of pointers (one extra for the terminating NULL) */
    char **lines = (char **)calloc(num_lines + 1, sizeof(char *));
    assert(lines != NULL);

    /* Fill the array with pointers to the start of each line */
    size_t line_idx = 0;
    char *line_start = buffer;

    for (size_t i = 0; buffer[i] != '\0'; i++) {
        if (buffer[i] == '\n') {
            /* Mark the end of the current line */
            buffer[i] = '\0';
            /* Store the pointer to the start of this line */
            lines[line_idx++] = line_start;
            /* Next line starts after the newline */
            line_start = &buffer[i + 1];
        }
    }
    /* Store the last line (which may be empty if the string ends with '\n') */
    lines[line_idx] = line_start;
    /* The next entry is already NULL because calloc zeroed the array */

    return lines;
}

char **split_words(char *line) {
    assert(line != NULL);

    /* --- First pass: count the number of words --- */
    size_t word_count = 0;
    char *p = line;
    while (*p != '\0') {
        /* Skip any leading spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;   /* end of string */
        /* Found the start of a word */
        word_count++;
        /* Skip the rest of the word until space or end */
        while (*p != '\0' && *p != ' ') p++;
    }

    /* Allocate the pointer array (one extra for the terminating NULL) */
    char **words = (char **)calloc(word_count + 1, sizeof(char *));
    assert(words != NULL);

    /* --- Second pass: fill the array and replace spaces --- */
    size_t idx = 0;
    p = line;
    while (*p != '\0') {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;
        /* Store pointer to the start of this word */
        words[idx++] = p;
        /* Move to the end of the word */
        while (*p != '\0' && *p != ' ') p++;
        /* If we stopped at a space, replace it with '\0' and advance */
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    /* The last entry is already NULL because calloc zeroed the array */

    return words;
}

char ***split_into_word_arrays(char *buffer) {
    assert(buffer != NULL);

    char **lines = split_lines(buffer);
    assert(lines != NULL);

    /* Count lines */
    size_t line_count = 0;
    while (lines[line_count] != NULL) {
        line_count++;
    }

    /* Allocate top‑level array */
    char ***word_arrays = (char ***)calloc(line_count + 1, sizeof(char **));
    assert(word_arrays != NULL);

    /* For each line, split into words and store */
    for (size_t i = 0; i < line_count; i++) {
        word_arrays[i] = split_words(lines[i]);
        assert(word_arrays[i] != NULL);
    }
    /* The last element is already NULL */

    free(lines);  /* we no longer need the temporary line array */
    return word_arrays;
}

void reverse_word_order(char **words) {
    assert(words != NULL);

    /* Find the length of the array (excluding the terminating NULL) */
    size_t len = 0;
    while (words[len] != NULL) {
        len++;
    }

    if (len == 0) {
        return;   /* empty array, nothing to reverse */
    }

    /* Swap pointers from both ends */
    size_t left = 0;
    size_t right = len - 1;
    while (left < right) {
        char *temp = words[left];
        words[left] = words[right];
        words[right] = temp;
        left++;
        right--;
    }
}

void reverse_word(char *word) {
    assert(word != NULL);

    char *end = word;
    /* Advance until we hit a space or the end of the string */
    while (*end != '\0' && *end != ' ') {
        end++;
    }
    /* 'end' now points to the terminator; the last character is end - 1 */
    char *left = word;
    char *right = end - 1;

    /* Swap characters from both ends until the pointers meet */
    while (left < right) {
        char temp = *left;
        *left = *right;
        *right = temp;
        left++;
        right--;
    }
}

void print_word_arrays(char ***arrays) {
    assert(arrays != NULL);

    for (size_t line = 0; arrays[line] != NULL; line++) {
        char **words = arrays[line];

        /* If the line is empty, just print a newline */
        if (words[0] == NULL) {
            printf("\n");
            continue;
        }

        /* Print each word, inserting spaces between them */
        for (size_t w = 0; words[w] != NULL; w++) {
            printf("%s", words[w]);
            if (words[w + 1] != NULL) {
                printf(" ");
            }
        }
        printf("\n");
    }
}

/* -------------------- Main program -------------------- */
int main(int argc, char *argv[]) {
    int word_index;

    char *buffer = read_argument(argc, argv, &word_index);
    char ***word_arrays = split_into_word_arrays(buffer);

    for (size_t line = 0; word_arrays[line] != NULL; line++) {
        char **words = word_arrays[line];

        /* Count words in this line */
        size_t word_count = 0;
        while (words[word_count] != NULL) {
            word_count++;
        }

        /* Reverse the word at 'word_index' only if it exists */
        if (word_index >= 0 && (size_t)word_index < word_count) {
            reverse_word(words[word_index]);
        }

        /* Then reverse the whole word order */
        reverse_word_order(words);
    }

    print_word_arrays(word_arrays);

    /* Free memory */
    for (size_t line = 0; word_arrays[line] != NULL; line++) {
        free(word_arrays[line]);
    }
    free(word_arrays);
    free(buffer);

    return 0;
}