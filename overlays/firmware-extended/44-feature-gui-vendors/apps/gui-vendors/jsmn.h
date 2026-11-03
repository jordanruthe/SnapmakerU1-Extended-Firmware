/*
 * jsmn - minimal JSON tokenizer (header-only, trimmed single-file copy).
 * Original: https://github.com/zserge/jsmn
 *
 * MIT License
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT = 1 << 0,
  JSMN_ARRAY = 1 << 1,
  JSMN_STRING = 1 << 2,
  JSMN_PRIMITIVE = 1 << 3
} jsmntype_t;

enum jsmnerr {
  JSMN_ERROR_NOMEM = -1,  /* Not enough tokens were provided */
  JSMN_ERROR_INVAL = -2,  /* Invalid character inside JSON string */
  JSMN_ERROR_PART = -3    /* The string is not a full JSON packet, more bytes expected */
};

typedef struct jsmntok {
  jsmntype_t type;
  int start;
  int end;
  int size;
  int parent;
} jsmntok_t;

typedef struct jsmn_parser {
  unsigned int pos;     /* offset in the JSON string */
  unsigned int toknext; /* next token to allocate */
  int toksuper;         /* superior token node, e.g. parent object or array */
} jsmn_parser;

static inline void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0;
  parser->toknext = 0;
  parser->toksuper = -1;
}

static inline jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                          const size_t num_tokens) {
  jsmntok_t *tok;
  if (parser->toknext >= num_tokens) {
    return NULL;
  }
  tok = &tokens[parser->toknext++];
  tok->start = tok->end = -1;
  tok->size = 0;
  tok->parent = -1;
  return tok;
}

static inline void jsmn_fill_token(jsmntok_t *token, const jsmntype_t type,
                                   const int start, const int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

static inline int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                       const size_t len, jsmntok_t *tokens,
                                       const size_t num_tokens) {
  jsmntok_t *token;
  int start = (int)parser->pos;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    switch (js[parser->pos]) {
    case '\t': case '\r': case '\n': case ' ':
    case ',': case ']': case '}':
      goto found;
    default:
      break;
    }
    if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
      parser->pos = (unsigned int)start;
      return JSMN_ERROR_INVAL;
    }
  }
  /* Strict mode: a primitive must be followed by a delimiter. */
  parser->pos = (unsigned int)start;
  return JSMN_ERROR_PART;

found:
  if (tokens == NULL) {
    parser->pos--;
    return 0;
  }
  token = jsmn_alloc_token(parser, tokens, num_tokens);
  if (token == NULL) {
    parser->pos = (unsigned int)start;
    return JSMN_ERROR_NOMEM;
  }
  jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
  token->parent = parser->toksuper;
  parser->pos--;
  return 0;
}

static inline int jsmn_parse_string(jsmn_parser *parser, const char *js,
                                    const size_t len, jsmntok_t *tokens,
                                    const size_t num_tokens) {
  jsmntok_t *token;
  int start = (int)parser->pos;

  parser->pos++;
  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c = js[parser->pos];

    if (c == '\"') {
      if (tokens == NULL) {
        return 0;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
      }
      jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
      token->parent = parser->toksuper;
      return 0;
    }

    if ((unsigned char)c < 32) {
      parser->pos = (unsigned int)start;
      return JSMN_ERROR_INVAL;
    }

    if (c == '\\' && parser->pos + 1 < len) {
      parser->pos++;
      switch (js[parser->pos]) {
      case '\"': case '/': case '\\': case 'b':
      case 'f': case 'r': case 'n': case 't':
        break;
      case 'u':
        parser->pos++;
        for (int i = 0; i < 4 && parser->pos < len && js[parser->pos] != '\0'; i++) {
          char h = js[parser->pos];
          if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                (h >= 'a' && h <= 'f'))) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
          }
          parser->pos++;
        }
        parser->pos--;
        break;
      default:
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
  parser->pos = (unsigned int)start;
  return JSMN_ERROR_PART;
}

/* Parse JSON string `js` of `len` bytes into tokens. With tokens == NULL only
 * counts the tokens needed. Returns the token count or a negative jsmnerr. */
static inline int jsmn_parse(jsmn_parser *parser, const char *js, const size_t len,
                             jsmntok_t *tokens, const unsigned int num_tokens) {
  int r;
  int i;
  jsmntok_t *token;
  int count = (int)parser->toknext;

  for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
    char c = js[parser->pos];
    jsmntype_t type;

    switch (c) {
    case '{': case '[':
      count++;
      if (tokens == NULL) {
        break;
      }
      token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        return JSMN_ERROR_NOMEM;
      }
      if (parser->toksuper != -1) {
        jsmntok_t *t = &tokens[parser->toksuper];
        t->size++;
        token->parent = parser->toksuper;
      }
      token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
      token->start = (int)parser->pos;
      parser->toksuper = (int)parser->toknext - 1;
      break;
    case '}': case ']':
      if (tokens == NULL) {
        break;
      }
      type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
      if (parser->toknext < 1) {
        return JSMN_ERROR_INVAL;
      }
      token = &tokens[parser->toknext - 1];
      for (;;) {
        if (token->start != -1 && token->end == -1) {
          if (token->type != type) {
            return JSMN_ERROR_INVAL;
          }
          token->end = (int)parser->pos + 1;
          parser->toksuper = token->parent;
          break;
        }
        if (token->parent == -1) {
          if (token->type != type || parser->toksuper == -1) {
            return JSMN_ERROR_INVAL;
          }
          break;
        }
        token = &tokens[token->parent];
      }
      break;
    case '\"':
      r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;
    case '\t': case '\r': case '\n': case ' ':
      break;
    case ':':
      parser->toksuper = (int)parser->toknext - 1;
      break;
    case ',':
      if (tokens != NULL && parser->toksuper != -1 &&
          tokens[parser->toksuper].type != JSMN_ARRAY &&
          tokens[parser->toksuper].type != JSMN_OBJECT) {
        parser->toksuper = tokens[parser->toksuper].parent;
      }
      break;
    default:
      /* numbers, true, false, null */
      if (!((c >= '0' && c <= '9') || c == '-' || c == 't' || c == 'f' || c == 'n')) {
        return JSMN_ERROR_INVAL;
      }
      if (tokens != NULL && parser->toksuper != -1) {
        const jsmntok_t *t = &tokens[parser->toksuper];
        if (t->type == JSMN_OBJECT || (t->type == JSMN_STRING && t->size != 0)) {
          return JSMN_ERROR_INVAL;
        }
      }
      r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
      if (r < 0) {
        return r;
      }
      count++;
      if (parser->toksuper != -1 && tokens != NULL) {
        tokens[parser->toksuper].size++;
      }
      break;
    }
  }

  if (tokens != NULL) {
    for (i = (int)parser->toknext - 1; i >= 0; i--) {
      /* Unmatched opened object or array */
      if (tokens[i].start != -1 && tokens[i].end == -1) {
        return JSMN_ERROR_PART;
      }
    }
  }

  return count;
}

#endif /* JSMN_H */
