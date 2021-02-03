/*
 * Copyright (C) 2015 SensorsData
 * All rights reserved.
 */

#include <string.h>
#include <stdint.h>

#if defined(USE_POSIX)
#include <pthread.h>
#include <regex.h>
#include <sys/time.h>
#elif defined(_WIN32)
#include <windows.h>
#include <sys/timeb.h>
#include <share.h>
#include "pcre/pcre.h"
#elif defined(__linux__)
#include <sys/time.h>
#endif

#include "sensors_analytics.h"

#define SA_LIB_VERSION "0.2.0"
#define SA_LIB "C"
#define SA_LIB_METHOD "code"

#define KEY_WORD_PATTERN "(^distinct_id$|^original_id$|^time$|^properties$|^id$|^first_id$|^second_id$|^users$|^events$|^event$|^user_id$|^date$|^datetime$)"
#define NAME_PATTERN "^[a-zA-Z_$][a-zA-Z0-9_$]{0,99}$"

#if defined(__linux__)
#define LOCALTIME(seconds, now) localtime_r((seconds), (now))
#define FOPEN(file, filename, option) do { \
  *(file) = fopen((filename), (option)); \
} while (0)

#elif defined(__APPLE__)
#define LOCALTIME(seconds, now) localtime_r((seconds), (now))
#define FOPEN(file, filename, option) do { \
  *(file) = fopen((filename), (option)); \
} while (0)

#elif defined(_WIN32)
#define LOCALTIME(seconds, now) localtime_s((now), (seconds))
#define FOPEN(file, filename, option) do { \
  *(file) = _fsopen((filename), (option), _SH_DENYNO); \
} while (0)

#endif

static void* _sa_safe_malloc(unsigned long n, unsigned long line) {
  void* p = malloc(n);
  if (!p) {
    fprintf(stderr, "[%s:%lu]Out of memory(%lu bytes)\n", __FILE__, line, (unsigned long)n);
    exit(SA_MALLOC_ERROR);
  }
  return p;
}
#define SA_SAFE_MALLOC(n) _sa_safe_malloc((n), __LINE__)

static char* _sa_strdup(const char *str) {
  int len = strlen(str);
  char *ret = (char*)SA_SAFE_MALLOC(len + 1);
  memcpy(ret, str, len);
  ret[len] = '\0';
  return ret;
}

// String buffer --------------------------------------------------------------

typedef struct {
  char *cur;
  char *end;
  char *start;
} SAStringBuffer;

static int _sa_sb_init(SAStringBuffer *sb) {
  sb->start = (char*)SA_SAFE_MALLOC(17);
  sb->cur = sb->start;
  sb->end = sb->start + 16;
  return SA_OK;
}

/* sb and need may be evaluated multiple times. */
#define _sa_sb_need(sb, need) do { \
  int res = SA_OK; \
  if ((sb)->end < (sb)->cur + (need)) \
  if (SA_OK != (res = _sa_sb_grow(sb, need))) \
  return res; \
} while (0)

static int _sa_sb_grow(SAStringBuffer *sb, unsigned long need) {
  size_t length = sb->cur - sb->start;
  size_t alloc = sb->end - sb->start;

  do {
    alloc *= 2;
  } while (alloc < length + need);

  sb->start = (char*) realloc(sb->start, alloc + 1);
  if (sb->start == NULL) {
    fprintf(stderr, "Out of memory.");
    exit(SA_MALLOC_ERROR);
  }
  sb->cur = sb->start + length;
  sb->end = sb->start + alloc;
  return SA_OK;
}

static int _sa_sb_put(SAStringBuffer *sb, const char *string_, unsigned long length) {
  _sa_sb_need(sb, length);
  memcpy(sb->cur, string_, length);
  sb->cur += length;
  return SA_OK;
}

#define _sa_sb_putc(sb, c) do { \
  int res = SA_OK; \
  if ((sb)->cur >= (sb)->end) \
  if (SA_OK != (res = _sa_sb_grow(sb, 1))) \
  return res; \
  *(sb)->cur++ = (c); \
} while (0)

static char *_sa_sb_finish(SAStringBuffer *sb, unsigned long* length) {
  *sb->cur = 0;
  *length = sb->cur - sb->start;
  return sb->start;
}

static void _sa_sb_free(SAStringBuffer *sb) {
  free(sb->start);
}


// SANode ---------------------------------------------------------------------

// 属性的数据类型.
enum SANodeTag {
  SA_BOOL,
  SA_NUMBER,
  SA_INT,
  SA_DATE,
  SA_STRING,
  SA_LIST,
  SA_DICT
};

struct SAListNode;

typedef struct SANode {
  // 引用计数，初始值为 1.
  unsigned int ref_count;

  // 属性的 key，以 \0 结尾.
  char* key;

  enum SANodeTag tag;
  union {
    int bool_;
    double number_;
    long long int_;
    struct {
      time_t seconds;
      int microseconds;
    } date_;
    char* string_;              // 字符串必须是 UTF-8 编码.
    struct SAListNode* array_;  // 数组的元素必须是 UTF-8 编码的字符串.
  };
} SANode;

typedef struct SAListNode {
  // 存储事件属性的单向链表.
  struct SAListNode* next;

  struct SANode* value;
} SAListNode;

static void _sa_free_node(struct SANode* node);

// 初始化事件属性或用户属性对象.
static struct SANode* _sa_malloc_node(enum SANodeTag tag, const char* key) {
  struct SANode* node = (struct SANode*)SA_SAFE_MALLOC(sizeof(SANode));
  memset(node, 0, sizeof(struct SANode));

  node->ref_count = 1;
  node->tag = tag;
  if (NULL != key) {
    node->key = _sa_strdup(key);
  }

  return node;
}

static struct SANode* _sa_init_bool_node(const char* key, int bool_) {
  struct SANode* node = _sa_malloc_node(SA_BOOL, key);
  if (NULL == node) {
    return NULL;
  }

  node->bool_ = bool_;

  return node;
}

static struct SANode* _sa_init_number_node(const char* key, double number_) {
  struct SANode* node = _sa_malloc_node(SA_NUMBER, key);
  if (NULL == node) {
    return NULL;
  }

  node->number_ = number_;

  return node;
}

static struct SANode* _sa_init_int_node(const char* key, long long int_) {
  struct SANode* node = _sa_malloc_node(SA_INT, key);
  if (NULL == node) {
    return NULL;
  }

  node->int_ = int_;

  return node;
}

static struct SANode* _sa_init_date_node(const char* key, time_t seconds, int microseconds) {
  struct SANode* node = _sa_malloc_node(SA_DATE, key);
  if (NULL == node) {
    return NULL;
  }

  node->date_.seconds = seconds;
  node->date_.microseconds = microseconds;

  return node;
}


static struct SANode* _sa_init_string_node(const char* key, const char* str, unsigned int length) {
  struct SANode* node = _sa_malloc_node(SA_STRING, key);
  if (NULL == node) {
    return NULL;
  }

  node->string_ = (char*)SA_SAFE_MALLOC(length + 1);
  memcpy(node->string_, str, length);
  node->string_[length] = 0;

  return node;
}

static struct SANode* _sa_init_list_node(const char* key) {
  return _sa_malloc_node(SA_LIST, key);
}

static struct SANode* _sa_init_dict_node(const char* key) {
  return _sa_malloc_node(SA_DICT, key);
}

static struct SANode* _sa_get_child(const char* key, const struct SANode* parent) {
  if (parent->tag != SA_DICT || NULL == key) {
    return NULL;
  }

  struct SAListNode* curr = parent->array_;

  while (NULL != curr) {
    struct SAListNode* next = curr->next;

    if (NULL != curr->value->key && 0 == strncmp(curr->value->key, key, 256)) {
      return curr->value;
    }

    curr = next;
  }

  return NULL;
}

static void _sa_remove_child(const char* key, struct SANode* parent) {
  if (parent->tag != SA_DICT && parent->tag != SA_LIST) {
    return;
  }

  struct SAListNode* prev = NULL;
  struct SAListNode* curr = parent->array_;

  while (NULL != curr) {
    struct SAListNode* next = curr->next;

    if (NULL == key || (
        (NULL != curr->value->key && 0 == strncmp(curr->value->key, key, 256)))) {
      if (NULL != prev) {
        prev->next = next;
      } else {
        parent->array_ = next;
      }
      _sa_free_node(curr->value);
      // SAListNode 对象只在这里 free.
      free(curr);
    } else {
      prev = curr;
    }

    curr = next;
  }
}


static struct SAListNode* _sa_add_child(struct SANode* child, struct SANode* parent) {
  if (SA_DICT != parent->tag && SA_LIST != parent->tag) {
    return NULL;
  }

  if (SA_DICT == parent->tag) {
    if (NULL == child->key) {
      return NULL;
    }
    _sa_remove_child(child->key, parent);
  }

  // SAListNode 对象只在这里 malloc.
  struct SAListNode* element = (struct SAListNode*)SA_SAFE_MALLOC(sizeof(struct SAListNode));

  element->next = parent->array_;
  parent->array_ = element;

  element->value = child;
  // 操作引用计数.
  ++element->value->ref_count;

  return element;
}


// 释放事件属性或用户属性对象.
static void _sa_free_node(struct SANode* node) {
  if ((--node->ref_count) == 0) {
    if (NULL != node->key) {
      free(node->key);
    }

    // 释放属性的值.
    switch(node->tag) {
    case SA_STRING:
      free(node->string_);
      break;
    case SA_LIST:
    case SA_DICT:
      _sa_remove_child(NULL/* remove all child */, node);
      break;
    default:
      // DO NOTHING
      break;
    }
    free(node);
  }
}

int _sa_dump_node(const struct SANode* node, SAStringBuffer* sb);

int _sa_dump_dict(const struct SANode* node, SAStringBuffer* sb) {
  _sa_sb_putc(sb, '{');

  struct SAListNode* child = node->array_;
  while (NULL != child) {
    _sa_sb_putc(sb, '"');
    _sa_sb_put(sb, child->value->key, strlen(child->value->key));
    _sa_sb_putc(sb, '"');
    _sa_sb_putc(sb, ':');
    _sa_dump_node(child->value, sb);

    child = child->next;
    if (child != NULL) {
      _sa_sb_putc(sb, ',');
    }
  }

  _sa_sb_putc(sb, '}');
  return SA_OK;
}

int _sa_dump_list(const struct SANode* node, SAStringBuffer* sb) {
  _sa_sb_putc(sb, '[');

  struct SAListNode* child = node->array_;
  while (NULL != child) {
    _sa_dump_node(child->value, sb);

    child = child->next;
    if (child != NULL) {
      _sa_sb_putc(sb, ',');
    }
  }

  _sa_sb_putc(sb, ']');
  return SA_OK;
}

/*
 * Validate a single UTF-8 character starting at @s.
 * The string must be null-terminated.
 *
 * If it's valid, return its length (1 thru 4).
 * If it's invalid or clipped, return 0.
 *
 * This function implements the syntax given in RFC3629, which is
 * the same as that given in The Unicode Standard, Version 6.0.
 *
 * It has the following properties:
 *
 *  * All codepoints U+0000..U+10FFFF may be encoded,
 *    except for U+D800..U+DFFF, which are reserved
 *    for UTF-16 surrogate pair encoding.
 *  * UTF-8 byte sequences longer than 4 bytes are not permitted,
 *    as they exceed the range of Unicode.
 *  * The sixty-six Unicode "non-characters" are permitted
 *    (namely, U+FDD0..U+FDEF, U+xxFFFE, and U+xxFFFF).
 */
static int sa_utf8_validate_cz(const char *s) {
  unsigned char c = *s++;

  if (c <= 0x7F) {        /* 00..7F */
    return 1;
  } else if (c <= 0xC1) { /* 80..C1 */
    /* Disallow overlong 2-byte sequence. */
    return 0;
  } else if (c <= 0xDF) { /* C2..DF */
    /* Make sure subsequent byte is in the range 0x80..0xBF. */
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }

    return 2;
  } else if (c <= 0xEF) { /* E0..EF */
    /* Disallow overlong 3-byte sequence. */
    if (c == 0xE0 && (unsigned char)*s < 0xA0) {
      return 0;
    }

    /* Disallow U+D800..U+DFFF. */
    if (c == 0xED && (unsigned char)*s > 0x9F) {
      return 0;
    }

    /* Make sure subsequent bytes are in the range 0x80..0xBF. */
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }

    return 3;
  } else if (c <= 0xF4) { /* F0..F4 */
    /* Disallow overlong 4-byte sequence. */
    if (c == 0xF0 && (unsigned char)*s < 0x90) {
      return 0;
    }

    /* Disallow codepoints beyond U+10FFFF. */
    if (c == 0xF4 && (unsigned char)*s > 0x8F) {
      return 0;
    }

    /* Make sure subsequent bytes are in the range 0x80..0xBF. */
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }
    if (((unsigned char)*s++ & 0xC0) != 0x80) {
      return 0;
    }

    return 4;
  } else {                /* F5..FF */
    return 0;
  }
}

/* Validate a null-terminated UTF-8 string. */
static SABool sa_utf8_validate(const char *s) {
  int len;

  for (; *s != 0; s += len) {
    len = sa_utf8_validate_cz(s);
    if (len == 0) {
      return SA_FALSE;
    }
  }

  return SA_TRUE;
}

/*
 * Read a single UTF-8 character starting at @s,
 * returning the length, in bytes, of the character read.
 *
 * This function assumes input is valid UTF-8,
 * and that there are enough characters in front of @s.
 */
static int utf8_read_char(const char *s, unsigned int *out) {
  const unsigned int *c = (const unsigned int*) s;

  if (c[0] <= 0x7F) {
    /* 00..7F */
    *out = c[0];
    return 1;
  } else if (c[0] <= 0xDF) {
    /* C2..DF (unless input is invalid) */
    *out = ((unsigned int)c[0] & 0x1F) << 6 |
      ((unsigned int)c[1] & 0x3F);
    return 2;
  } else if (c[0] <= 0xEF) {
    /* E0..EF */
    *out = ((unsigned int)c[0] &  0xF) << 12 |
      ((unsigned int)c[1] & 0x3F) << 6  |
      ((unsigned int)c[2] & 0x3F);
    return 3;
  } else {
    /* F0..F4 (unless input is invalid) */
    *out = ((unsigned int)c[0] &  0x7) << 18 |
      ((unsigned int)c[1] & 0x3F) << 12 |
      ((unsigned int)c[2] & 0x3F) << 6  |
      ((unsigned int)c[3] & 0x3F);
    return 4;
  }
}


/*
 * Encodes a 16-bit number into hexadecimal,
 * writing exactly 4 hex chars.
 */
static int _sa_write_hex16(char *out, uint16_t val) {
  const char *hex = "0123456789ABCDEF";

  *out++ = hex[(val >> 12) & 0xF];
  *out++ = hex[(val >> 8)  & 0xF];
  *out++ = hex[(val >> 4)  & 0xF];
  *out++ = hex[ val        & 0xF];

  return 4;
}

/*
 * Construct a UTF-16 surrogate pair given a Unicode codepoint.
 *
 * @unicode must be U+10000..U+10FFFF.
 */
static void _sa_to_surrogate_pair(unsigned int unicode, uint16_t *uc, uint16_t *lc) {
  unsigned int n = unicode - 0x10000;
  *uc = ((n >> 10) & 0x3FF) | 0xD800;
  *lc = (n & 0x3FF) | 0xDC00;
}

int _sa_dump_string(const struct SANode* node, SAStringBuffer* sb) {
  SABool escape_unicode = SA_FALSE;
  const char *s = node->string_;
  char *b;

  if (!sa_utf8_validate(s)) {
    fprintf(stderr, "Invalid utf-8 string.");
    return SA_INVALID_PARAMETER_ERROR;
  }

  /*
   * 14 bytes is enough space to write up to two
   * \uXXXX escapes and two quotation marks.
   */
  _sa_sb_need(sb, 14);
  b = sb->cur;

  *b++ = '"';
  while (*s != 0) {
    unsigned char c = *s++;

    /* Encode the next character, and write it to b. */
    switch (c) {
    case '"':
      *b++ = '\\';
      *b++ = '"';
      break;
    case '\\':
      *b++ = '\\';
      *b++ = '\\';
      break;
    case '\b':
      *b++ = '\\';
      *b++ = 'b';
      break;
    case '\f':
      *b++ = '\\';
      *b++ = 'f';
      break;
    case '\n':
      *b++ = '\\';
      *b++ = 'n';
      break;
    case '\r':
      *b++ = '\\';
      *b++ = 'r';
      break;
    case '\t':
      *b++ = '\\';
      *b++ = 't';
      break;
    default: {
      int len;

      s--;
      len = sa_utf8_validate_cz(s);

      if (len == 0) {
        /*
         * Handle invalid UTF-8 character gracefully in production
         * by writing a replacement character (U+FFFD)
         * and skipping a single byte.
         *
         * This should never happen when assertions are enabled
         * due to the assertion at the beginning of this function.
         */
        if (escape_unicode) {
          memcpy(b, "\\uFFFD", 6);
          b += 6;
        } else {
          *b++ = 0xEF;
          *b++ = 0xBF;
          *b++ = 0xBD;
        }
        s++;
      } else if (c < 0x1F || (c >= 0x80 && escape_unicode)) {
        /* Encode using \u.... */
        unsigned int unicode;

        s += utf8_read_char(s, &unicode);

        if (unicode <= 0xFFFF) {
          *b++ = '\\';
          *b++ = 'u';
          b += _sa_write_hex16(b, unicode);
        } else {
          /* Produce a surrogate pair. */
          uint16_t uc, lc;
          // assert(unicode <= 0x10FFFF);
          _sa_to_surrogate_pair(unicode, &uc, &lc);
          *b++ = '\\';
          *b++ = 'u';
          b += _sa_write_hex16(b, uc);
          *b++ = '\\';
          *b++ = 'u';
          b += _sa_write_hex16(b, lc);
        }
      } else {
        /* Write the character directly. */
        while (len--)
          *b++ = *s++;
      }

      break;
    }
    }

    /*
     * Update *sb to know about the new bytes,
     * and set up b to write another encoded character.
     */
    sb->cur = b;
    _sa_sb_need(sb, 14);
    b = sb->cur;
  }
  *b++ = '"';

  sb->cur = b;
  return SA_OK;
}

// 将 struct SANode JSON 序列化至文件.
int _sa_dump_node(const struct SANode* node, SAStringBuffer* sb) {
  if (NULL == node || NULL == sb) {
    return SA_INVALID_PARAMETER_ERROR;
  }

  char buf[64];
  struct tm tm;

  switch(node->tag) {
  case SA_BOOL:
    if (node->bool_) {
      _sa_sb_put(sb, "true", strlen("true"));
    } else {
      _sa_sb_put(sb, "false", strlen("false"));
    }
    break;
  case SA_NUMBER:
    snprintf(buf, 64, "%.3f", node->number_);
    _sa_sb_put(sb, buf, strlen(buf));
    break;
  case SA_INT:
    snprintf(buf, 64, "%lld", node->int_);
    _sa_sb_put(sb, buf, strlen(buf));
    break;
  case SA_DATE:
    LOCALTIME(&node->date_.seconds, &tm);
    snprintf(buf, 64, "\"%04d-%02d-%02d %02d:%02d:%02d.%03d\"",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             node->date_.microseconds);
    _sa_sb_put(sb, buf, strlen(buf));
    break;
  case SA_STRING:
    _sa_dump_string(node, sb);
    break;
  case SA_LIST:
    _sa_dump_list(node, sb);
    break;
  case SA_DICT:
    _sa_dump_dict(node, sb);
    break;
  default:
    return SA_INVALID_PARAMETER_ERROR;
  }

  return SA_OK;
}

// Properites -----------------------------------------------------------------

SAProperties* sa_init_properties() {
  return _sa_init_dict_node("properties");
}

void sa_free_properties(SAProperties* properties) {
  _sa_free_node(properties);
}

int sa_add_bool(const char* key, SABool bool_, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  struct SANode* child = _sa_init_bool_node(key, bool_);
  if (NULL == child) {
    fprintf(stderr, "Out of memory.");
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(child, properties);

  _sa_free_node(child);
  return SA_OK;
}

int sa_add_number(const char* key, double number_, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  struct SANode* child = _sa_init_number_node(key, number_);
  if (NULL == child) {
    fprintf(stderr, "Out of memory.");
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(child, properties);

  _sa_free_node(child);
  return SA_OK;
}

int sa_add_int(const char* key, long long int_, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  struct SANode* child = _sa_init_int_node(key, int_);
  if (NULL == child) {
    fprintf(stderr, "Out of memory.");
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(child, properties);

  _sa_free_node(child);
  return SA_OK;
}

int sa_add_date(const char* key, time_t seconds, int microseconds, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  struct SANode* child = _sa_init_date_node(key, seconds, microseconds);
  if (NULL == child) {
    fprintf(stderr, "Out of memory.");
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(child, properties);

  _sa_free_node(child);
  return SA_OK;
}

int sa_add_string(const char* key, const char* string_, unsigned int length, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  struct SANode* child = _sa_init_string_node(key, string_, length);
  if (NULL == child) {
    fprintf(stderr, "Out of memory.");
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(child, properties);

  _sa_free_node(child);
  return SA_OK;
}

// 向事件属性或用户属性的 List 类型的属性中插入新对象，对象必须是 String 类型的.
int sa_append_list(const char* key, const char* string_, unsigned int length, SAProperties* properties) {
  if (NULL == properties) {
    fprintf(stderr, "Parameter 'properties' is NULL.");
    return SA_INVALID_PARAMETER_ERROR;
  }
  // TODO: check key

  // 向 properties 中添加 List 对象，若该对象已存在，则使用该对象.
  struct SANode* list = _sa_get_child(key, properties);
  if (NULL == list) {
    list = _sa_init_list_node(key);
    if (NULL == list) {
      return SA_MALLOC_ERROR;
    }
    _sa_add_child(list, properties);

  } else {
    ++list->ref_count;
  }

  // 向 List 对象中添加字符串属性.
  struct SANode* str_child = _sa_init_string_node(NULL, string_, length);
  if (NULL == str_child) {
    return SA_MALLOC_ERROR;
  }

  _sa_add_child(str_child, list);

  _sa_free_node(str_child);

  _sa_free_node(list);

  return SA_OK;
}

// Logging Consumer -----------------------------------------------------------

typedef struct {
  char file_name[512];
  char file_name_prefix[512];
  // 日志文件日期，存为数字，20170101.
  int date;
  // 输出文件句柄.
  FILE* file;
} SALoggingConsumerInter;

static int _sa_get_current_date() {
  time_t t = time(NULL);
  struct tm tm;
  LOCALTIME(&t, &tm);
  return tm.tm_year * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static int _sa_logging_consumer_flush(void* this_) {
  SALoggingConsumerInter* inter = (SALoggingConsumerInter*)this_;
  if (NULL != inter->file && 0 == fflush(inter->file)) {
    return SA_OK;
  }
  return SA_IO_ERROR;
}

static int _sa_logging_consumer_close(void* this_) {
  if (NULL == this_) {
    return SA_INVALID_PARAMETER_ERROR;
  }

  SALoggingConsumerInter* inter = (SALoggingConsumerInter*)this_;
  if (NULL != inter->file) {
    fflush(inter->file);
    fclose(inter->file);
    inter->file = NULL;
  }

  return SA_OK;
}

static int _sa_logging_consumer_send(void* this_, const char* event, unsigned long length) {
  if (NULL == this_ || NULL == event) {
    return SA_INVALID_PARAMETER_ERROR;
  }

  SALoggingConsumerInter* inter = (SALoggingConsumerInter*)this_;

  // 判断日志文件的日期是否为当日.
  int date = _sa_get_current_date();
  if (date != inter->date) {
    _sa_logging_consumer_close(this_);

    inter->date = date;
    snprintf(inter->file_name, 512, "%s.log.%d", inter->file_name_prefix, date);

    // Append 模式打开文件.
    FOPEN(&inter->file, inter->file_name, "a");
    if (NULL == inter->file) {
      fprintf(stderr, "Failed to open file.");
      return SA_IO_ERROR;
    }
  }

  fwrite(event, length, 1, inter->file);
  fwrite("\n", 1, 1, inter->file);

  return SA_OK;
}

// 初始化 Logging Consumer.
int sa_init_logging_consumer(const char* file_name, SALoggingConsumer** sa) {
  if (strlen(file_name) > 500) {
    fprintf(stderr,"The file name length must not exceed 500.");
    return SA_INVALID_PARAMETER_ERROR;
  }

  SALoggingConsumerInter* inter = (SALoggingConsumerInter*)SA_SAFE_MALLOC(sizeof(SALoggingConsumerInter));

  memset(inter, 0, sizeof(SALoggingConsumerInter));
  memcpy(inter->file_name_prefix, file_name, strlen(file_name));

  *sa = (SALoggingConsumer*)SA_SAFE_MALLOC(sizeof(SALoggingConsumer));

  (*sa)->this_ = (void*)inter;
  (*sa)->op.send = &_sa_logging_consumer_send;
  (*sa)->op.flush = &_sa_logging_consumer_flush;
  (*sa)->op.close = &_sa_logging_consumer_close;

  return SA_OK;
}

// Sensors Analytics ----------------------------------------------------------
typedef struct SensorsAnalytics {
  // 存储事件公共属性.
  SAProperties* super_properties;
#if defined(USE_POSIX)
  // Mutex
  pthread_mutex_t mutex;
  // 检查事件、属性规范的正则表达式.
  regex_t regex[2];
#elif defined(_WIN32)
  CRITICAL_SECTION mutex;
  pcre* regex[2];
#endif
  struct SAConsumer* consumer;
} SensorsAnalytics;

int sa_init(struct SAConsumer* consumer, SensorsAnalytics** sa) {
  *sa = (SensorsAnalytics*)SA_SAFE_MALLOC(sizeof(SensorsAnalytics));

  (*sa)->super_properties = sa_init_properties();
  if (NULL == (*sa)->super_properties) {
    free(*sa);
    return SA_MALLOC_ERROR;
  }

#if defined(USE_POSIX)
  if (pthread_mutex_init(&((*sa)->mutex), NULL) != 0) {
    fprintf(stderr, "Initialize mutex error.");
    return SA_MALLOC_ERROR;
  }

  if (0 != regcomp(&((*sa)->regex[0]), KEY_WORD_PATTERN, REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
    fprintf(stderr, "Compile regex error.");
    return SA_MALLOC_ERROR;
  }
  if (0 != regcomp(&((*sa)->regex[1]), NAME_PATTERN, REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
    fprintf(stderr, "Compile regex error.");
    return SA_MALLOC_ERROR;
  }
#elif defined(_WIN32)
  InitializeCriticalSection(&((*sa)->mutex));

  const char* error_message = NULL;
  int offset = -1;
  if (NULL == ((*sa)->regex[0] = pcre_compile(
        KEY_WORD_PATTERN, PCRE_EXTENDED | PCRE_CASELESS, &error_message, &offset, NULL))) {
    fprintf(stderr, "Compile regex error. ErrMsg:%s, Offset:%d", error_message, offset);
    return SA_MALLOC_ERROR;
  }
  if (NULL == ((*sa)->regex[1] = pcre_compile(
        NAME_PATTERN, PCRE_EXTENDED | PCRE_CASELESS, &error_message, &offset, NULL))) {
    fprintf(stderr, "Compile regex error. ErrMsg:%s, Offset:%d", error_message, offset);
    return SA_MALLOC_ERROR;
  }
#endif

  (*sa)->consumer = consumer;

  return SA_OK;
}

// 释放 sa.
void sa_free(SensorsAnalytics* sa) {
  if (NULL == sa) {
    return;
  }

  sa_free_properties(sa->super_properties);

#if defined(USE_POSIX)
  pthread_mutex_destroy(&(sa->mutex));

  regfree(&(sa->regex[0]));
  regfree(&(sa->regex[1]));
#elif defined(_WIN32)
  DeleteCriticalSection(&(sa->mutex));
  pcre_free(sa->regex[0]);
  pcre_free(sa->regex[1]);
#endif

  sa->consumer->op.close(sa->consumer->this_);

  free(sa);
}

// 同步 sa 的状态，将发送 sa 的缓存中所有数据.
void sa_flush(SensorsAnalytics* sa) {
  sa->consumer->op.flush(sa->consumer->this_);
}

int sa_register_super_properties(const SAProperties* properties, SensorsAnalytics *sa) {
  if (NULL == sa || NULL == properties || SA_DICT != properties->tag) {
    return SA_INVALID_PARAMETER_ERROR;
  }

  // 遍历 properties 中所有属性，逐个保存在 super properties 中.
#if defined(USE_POSIX)
  pthread_mutex_lock(&sa->mutex);
#elif defined(_WIN32)
  EnterCriticalSection(&sa->mutex);
#endif
  struct SAListNode* curr = properties->array_;
  while (NULL != curr) {
    _sa_add_child(curr->value, sa->super_properties);
    curr = curr->next;
  }
#if defined(USE_POSIX)
  pthread_mutex_unlock(&sa->mutex);
#elif defined(_WIN32)
  LeaveCriticalSection(&sa->mutex);
#endif

  return SA_OK;
}

int sa_unregister_super_properties(const char* key, SensorsAnalytics *sa) {
#if defined(USE_POSIX)
  pthread_mutex_lock(&sa->mutex);
#elif defined(_WIN32)
  EnterCriticalSection(&sa->mutex);
#endif
  _sa_remove_child(key, sa->super_properties);
#if defined(USE_POSIX)
  pthread_mutex_unlock(&sa->mutex);
#elif defined(_WIN32)
  LeaveCriticalSection(&sa->mutex);
#endif
  return SA_OK;
}

int sa_clear_super_properties(SensorsAnalytics *sa) {
#if defined(USE_POSIX)
  pthread_mutex_lock(&sa->mutex);
#elif defined(_WIN32)
  EnterCriticalSection(&sa->mutex);
#endif
  _sa_remove_child(NULL, sa->super_properties);
#if defined(USE_POSIX)
  pthread_mutex_unlock(&sa->mutex);
#elif defined(_WIN32)
  LeaveCriticalSection(&sa->mutex);
#endif
  return SA_OK;
}

// Track events ---------------------------------------------------------------

#if defined(USE_POSIX)
static int _sa_assert_key_name(const char* key, regex_t* regex) {
#elif defined(_WIN32)
static int _sa_assert_key_name(const char* key, pcre** regex) {
#else
static int _sa_assert_key_name(const char* key) {
#endif
  unsigned long key_len = (NULL == key ? (unsigned long)-1 : strlen(key));
  if (key_len < 1 || key_len > 255) {
    return SA_INVALID_PARAMETER_ERROR;
  }
#if defined(USE_POSIX)
  if (0 == regexec(&regex[0], key, 0, NULL, 0)) {
    // Match keywords
    return SA_INVALID_PARAMETER_ERROR;
  }
  if (0 != regexec(&regex[1], key, 0, NULL, 0)) {
    // Match keywords
    return SA_INVALID_PARAMETER_ERROR;
  }
#elif defined(_WIN32)
  // the match case
  if (0 <= pcre_exec(regex[0], NULL, key, strlen(key), 0, 0, NULL, 0)) {
    return SA_INVALID_PARAMETER_ERROR;
  }
  if (0 > pcre_exec(regex[1], NULL, key, strlen(key), 0, 0, NULL, 0)) {
    return SA_INVALID_PARAMETER_ERROR;
  }

#endif
  return SA_OK;
}

static int _sa_is_track(const char* type) {
  return 0 == strncmp(type, "track", strlen("track"));
}

static int _sa_is_track_signup(const char* type) {
  return 0 == strncmp(type, "track_signup", strlen("track_signup"));
}

static int _sa_check_legality(
  const char* distinct_id,
  const char* origin_id,
  const char* type,
  const char* event,
  const struct SANode* properties,
  SensorsAnalytics* sa) {
  // 合法性检查.
  unsigned long distinct_id_len = (NULL == distinct_id ? (unsigned long)-1 : strlen(distinct_id));
  if (distinct_id_len < 1 || distinct_id_len > 255) {
    fprintf(
      stderr,
      "Invalid distinct id [%s].\n",
      distinct_id == NULL ? "NULL" : distinct_id);
    return SA_INVALID_PARAMETER_ERROR;
  }
  if (_sa_is_track_signup(type)) {
    unsigned long origin_id_len = (NULL == origin_id ? (unsigned long)-1 : strlen(origin_id));
    if (origin_id_len < 1 || origin_id_len > 255) {
      fprintf(
        stderr,
        "Invalid original distinct id [%s].\n",
        origin_id == NULL ? "NULL" : origin_id);
      return SA_INVALID_PARAMETER_ERROR;
    }
  }
  if (_sa_is_track(type) &&
      (NULL == event
#if defined(USE_POSIX)
    || SA_OK != _sa_assert_key_name(event, sa->regex))) {
#elif defined(_WIN32)
    || SA_OK != _sa_assert_key_name(event, sa->regex))) {
#else
    || SA_OK != _sa_assert_key_name(event))) {
#endif
    fprintf(stderr, "Invalid event name [%s].\n", event == NULL ? "NULL" : event);
    return SA_INVALID_PARAMETER_ERROR;
  }
  if (NULL != properties) {
    SAListNode* curr = properties->array_;
    while (NULL != curr) {
      if (NULL == curr->value->key
#if defined(USE_POSIX)
        || SA_OK != _sa_assert_key_name(curr->value->key, sa->regex)) {
#elif defined(_WIN32)
        || SA_OK != _sa_assert_key_name(curr->value->key, sa->regex)) {
#else
        || SA_OK != _sa_assert_key_name(curr->value->key)) {
#endif
        fprintf(stderr, "Invalid property name [%s].\n",
          NULL == curr->value->key ? "NULL" : curr->value->key);
        return SA_INVALID_PARAMETER_ERROR;
      }
      curr = curr->next;
    }
  }
  return SA_OK;
}

static int _sa_track_internal(
  const char* distinct_id,
  const char* origin_id,
  const char* type,
  const char* event,
  const struct SANode* properties,
  const char* __file__,
  const char* __function__,
  unsigned long __line__,
  SensorsAnalytics* sa) {
  int res = SA_OK;

  // 合法性检查.
  if (SA_OK != (res = _sa_check_legality(distinct_id, origin_id, type, event, properties, sa))) {
    return res;
  }

  // msg 记录一个事件，例如: {"type" : "track", "event" : "AppStart", "distinct_id" : "12345", "properties" : { ... }, ...}
  SANode* msg = _sa_init_dict_node(NULL);

  // 写入 type 字段.
  if (SA_OK != (res = sa_add_string("type", type, strlen(type), msg))) {
    return res;
  }
  // TODO: 写入 lib 字段
  // 写入 distinct id.
  if (SA_OK != (res = sa_add_string("distinct_id", distinct_id, strlen(distinct_id), msg))) {
    return res;
  }
  // 写入 origin id.
  if (_sa_is_track_signup(type)) {
    if (SA_OK != (res = sa_add_string("original_id", origin_id, strlen(origin_id), msg))) {
      return res;
    }
  }
  // 写入 event 字段.
  if (_sa_is_track(type) || _sa_is_track_signup(type)) {
    sa_add_string("event", event, strlen(event), msg);
  }

  // 写入 time 字段.
#if defined(USE_POSIX)
  struct timeval now;
  gettimeofday(&now, NULL);
  if (SA_OK != (res = sa_add_int("time", (long long)now.tv_sec * 1000 + now.tv_usec / 1000, msg))) {
    return res;
  }
#elif defined(_WIN32)
  struct timeb now;
  ftime(&now);
  if (SA_OK != (res = sa_add_int("time", (long long)now.time * 1000 + now.millitm, msg))) {
    return res;
  }
#elif defined(__linux__)
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (SA_OK != (res = sa_add_int("time", (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000, msg))) {
    return res;
  }
#else
  time_t now = time(NULL);
  if (SA_OK != (res = sa_add_int("time", (long long)now * 1000, msg))) {
    return res;
  }
#endif

  // 埋点管理信息
  // "lib":{"$lib_method":"code","$lib_detail":"testMethod##testDebug##test_sdk.py##60","$lib_version":"1.5.1","$lib":"python"}
  SANode* lib_properties = _sa_init_dict_node("lib");

  sa_add_string("$lib", SA_LIB, strlen(SA_LIB), lib_properties);
  sa_add_string("$lib_version", SA_LIB_VERSION, strlen(SA_LIB_VERSION), lib_properties);
  sa_add_string("$lib_method", SA_LIB_METHOD, strlen(SA_LIB_METHOD), lib_properties);
  char lib_detail_buf[256];
  snprintf(lib_detail_buf, 256, "##%s##%s##%ld", __function__, __file__, __line__);
  sa_add_string("$lib_detail", lib_detail_buf, strlen(lib_detail_buf), lib_properties);

  _sa_add_child(lib_properties, msg);

  sa_free_properties(lib_properties);

  // 事件属性.
  SANode* inner_properties = _sa_init_dict_node("properties");
  if (NULL == inner_properties) {
    return SA_MALLOC_ERROR;
  }

  if (_sa_is_track(type) || _sa_is_track_signup(type)) {
    // 属性中加入 $lib 和 $lib_version.
    sa_add_string("$lib", SA_LIB, strlen(SA_LIB), inner_properties);
    sa_add_string("$lib_version", SA_LIB_VERSION, strlen(SA_LIB_VERSION), inner_properties);

#if defined(USE_POSIX)
    pthread_mutex_lock(&sa->mutex);
#elif defined(_WIN32)
    EnterCriticalSection(&sa->mutex);
#endif
    SAListNode* curr = sa->super_properties->array_;
    while (NULL != curr) {
      _sa_add_child(curr->value, inner_properties);
      curr = curr->next;
    }
#if defined(USE_POSIX)
    pthread_mutex_unlock(&sa->mutex);
#elif defined(_WIN32)
    LeaveCriticalSection(&sa->mutex);
#endif
  }

  // 浅拷贝传入的 properties.
  if (NULL != properties) {
    SAListNode* curr = properties->array_;
    while (NULL != curr) {
      if (NULL != curr->value->key && 0 == strncmp("$time", curr->value->key, 256)) {
        // 若属性中包含 "$time" 对象，则使用它作为事件时间.
        SANode* time_node = _sa_get_child("$time", properties);
        if (NULL != time_node) {
          if (SA_OK != (res = sa_add_int(
                "time",
                (long)(time_node->date_.seconds) * 1000
                + (long)time_node->date_.microseconds / 1000,
                msg))) {
            return res;
          }
        }
      } else if (NULL != curr->value->key && 0 == strncmp("$project", curr->value->key, 256)) {
        // 若属性中包含 "$project" 对象，则将其改写为 project 属性.
        SANode* project_node = _sa_get_child("$project", properties);
        if (NULL != project_node) {
          if (SA_OK != (res = sa_add_string(
                "project",
                project_node->string_,
                strlen(project_node->string_),
                msg))) {
            return res;
          }
        }
      } else {
        _sa_add_child(curr->value, inner_properties);
      }
      curr = curr->next;
    }
  }

  // 写入 properties 字段.
  _sa_add_child(inner_properties, msg);

  sa_free_properties(inner_properties);

  // 序列化为字符串.
  SAStringBuffer sb;
  if (SA_OK != (res = _sa_sb_init(&sb))) {
    return res;
  }

  _sa_dump_node(msg, &sb);

  unsigned long msg_length = 0;
  const char* msg_str = _sa_sb_finish(&sb, &msg_length);

  // 使用 sa 发送事件.
  res = sa->consumer->op.send(sa->consumer->this_, msg_str, msg_length);

  _sa_sb_free(&sb);

  _sa_free_node(msg);

  return res;
}

int _sa_track(
        const char* distinct_id,
        const char* event,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  return _sa_track_internal(distinct_id,
                            NULL,
                            "track",
                            event,
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_track_signup(
        const char* distinct_id,
        const char* origin_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  return _sa_track_internal(distinct_id,
                            origin_id,
                            "track_signup",
                            "$SignUp",
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_profile_set(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  if (NULL == properties) {
    return SA_INVALID_PARAMETER_ERROR;
  }
  return _sa_track_internal(distinct_id,
                            NULL,
                            "profile_set",
                            NULL,
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_profile_set_once(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  if (NULL == properties) {
    return SA_INVALID_PARAMETER_ERROR;
  }
  return _sa_track_internal(distinct_id,
                            NULL,
                            "profile_set_once",
                            NULL,
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_profile_increment(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  if (NULL == properties) {
    return SA_INVALID_PARAMETER_ERROR;
  }
  return _sa_track_internal(distinct_id,
                            NULL,
                            "profile_increment",
                            NULL,
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_profile_append(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  if (NULL == properties) {
    return SA_INVALID_PARAMETER_ERROR;
  }
  return _sa_track_internal(distinct_id,
                            NULL,
                            "profile_append",
                            NULL,
                            properties,
                            __file__,
                            __function__,
                            __line__,
                            sa);
}

int _sa_profile_unset(
        const char* distinct_id,
        const char* key,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  int res = SA_OK;

  SAProperties *properties = sa_init_properties();
  if (NULL == properties) {
    return SA_MALLOC_ERROR;
  }

  sa_add_bool(key, SA_TRUE, properties);

  res = _sa_track_internal(distinct_id,
                           NULL,
                           "profile_unset",
                           NULL,
                           properties,
                           __file__,
                           __function__,
                           __line__,
                           sa);

  sa_free_properties(properties);

  return res;
}

int _sa_profile_delete(
        const char* distinct_id,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        SensorsAnalytics* sa) {
  int res = SA_OK;

  SAProperties *properties = sa_init_properties();
  if (NULL == properties) {
    return SA_MALLOC_ERROR;
  }

  res = _sa_track_internal(distinct_id,
                           NULL,
                           "profile_delete",
                           NULL,
                           properties,
                           __file__,
                           __function__,
                           __line__,
                           sa);

  sa_free_properties(properties);

  return res;
}


