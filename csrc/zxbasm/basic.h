/*
 * basic.h — Faithful C port of src/zxbasm/basic.py (class Basic)
 *
 * S6.3b: the ZX Spectrum BASIC-loader generator. Builds the byte stream
 * for a tokenised BASIC program (the .tap PROGRAM loader block). This is
 * a byte-for-byte port of basic.py's Basic class.
 *
 * NOTE: parse_sentence (basic.py:109-135) is intentionally NOT ported —
 * it is dead, unused, broken Python (never called).
 *
 * Self-contained: a local growable byte buffer. No new shared deps and
 * no cross-lib dependency on csrc/zxbc (the FP encoder is duplicated
 * verbatim into basic.c).
 */
#ifndef ZXBASM_BASIC_H
#define ZXBASM_BASIC_H

#include <stddef.h>

/* Opaque BASIC-program builder state (mirrors Python's Basic instance:
 * self.bytes = [] and self.current_line = 0). */
typedef struct Basic Basic;

/* Basic() — allocate a new builder. Returns NULL on OOM. */
Basic *basic_new(void);

/* Free the builder and its buffer. NULL-safe. */
void basic_free(Basic *b);

/* ----------------------------------------------------------------
 * Sentence model.
 *
 * Python: a "sentence" is a Python list whose 1st element is a string
 * that MUST match a TOKENS key, and whose remaining items are each
 * either a str (literal), an int/float (number), or "another thing"
 * (already a byte list, e.g. the result of token()).
 *
 * In C we model a sentence as an ordered list of typed items built
 * incrementally:
 *   - basic_sentence_new(token_word)  -> the mandatory 1st token item
 *   - basic_sentence_add_string(s)    -> a str literal item
 *   - basic_sentence_add_number(n)    -> an int/float number item
 *   - basic_sentence_add_token(word)  -> a RAW byte-list item == token(word)
 *   - basic_sentence_add_raw(bytes,n) -> a RAW byte-list item (generic)
 *
 * A "line" is an ordered list of sentences. Build with basic_line_new(),
 * append sentences with basic_line_add_sentence(), then commit with
 * basic_add_line()/basic_add_line_numbered().
 * ---------------------------------------------------------------- */

typedef struct BasicSentence BasicSentence;
typedef struct BasicLine BasicLine;

/* token_word: the mandatory first element — looked up in TOKENS
 * (upper-cased). Returns NULL on OOM. */
BasicSentence *basic_sentence_new(const char *token_word);

/* Append a string-literal item: extend(literal(s)) — raw bytes ord(c). */
void basic_sentence_add_string(BasicSentence *s, const char *str);

/* Append a number item: extend(number(n)) (int or float form). */
void basic_sentence_add_number(BasicSentence *s, double n);

/* Append a RAW byte-list item equal to Python token(word) == [TOKENS[word]]. */
void basic_sentence_add_token(BasicSentence *s, const char *word);

/* Append a generic RAW byte-list item (extend(item)). */
void basic_sentence_add_raw(BasicSentence *s, const unsigned char *bytes,
                            size_t n);

void basic_sentence_free(BasicSentence *s);

/* A line: an ordered list of sentences. Returns NULL on OOM. */
BasicLine *basic_line_new(void);

/* Append a sentence to the line. Ownership of the sentence transfers to
 * the line (freed by basic_line_free / after commit). */
void basic_line_add_sentence(BasicLine *line, BasicSentence *s);

void basic_line_free(BasicLine *line);

/* add_line(sentences) — line number = current_line + 10 (then commit).
 * Consumes (frees) the line. */
void basic_add_line(Basic *b, BasicLine *line);

/* add_line(sentences, line_number) — explicit line number.
 * Consumes (frees) the line. */
void basic_add_line_numbered(Basic *b, BasicLine *line, int line_number);

/* The accumulated program bytes (self.bytes). *len receives the length.
 * Valid until basic_free(). */
const unsigned char *basic_bytes(Basic *b, int *len);

/* 1 if an allocation failed at any point (sticky). */
int basic_oom(const Basic *b);

#endif /* ZXBASM_BASIC_H */
