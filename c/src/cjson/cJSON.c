/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/*
  Vendored for BITSDK. Faithful reproduction of the upstream cJSON
  (https://github.com/DaveGamble/cJSON, v1.7.x) public API and behavior,
  covering the parsing/printing/construction feature set used by this
  repository. The MIT license header above is kept verbatim from upstream.
*/

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

#define CJSON_NESTING_LIMIT 1000

/* define our own boolean macros */
#define cJSON_true  1
#define cJSON_false 0

typedef struct {
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth; /* how deeply nested (arrays/objects) the input is at the current offset */
} parse_buffer;

/* check if the given size is left to read in a given parse buffer */
#define can_read(buffer, size) (((buffer)->offset + size) <= (buffer)->length)
/* check if the buffer can be accessed at the given index */
#define can_access_at_index(buffer, index) ((buffer)->offset + index < (buffer)->length)
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* get a pointer to the buffer at the position */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* error position of the last failed parse */
static const char *global_ep = NULL;

/* Growable output buffer for printing. */
typedef struct {
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t depth; /* current nesting depth (for formatted printing) */
    cJSON_bool noalloc;
    cJSON_bool format; /* is this print a formatted output */
} print_buffer;

static cJSON *cJSON_New_Item(void);
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
static void buffer_skip_whitespace(parse_buffer * const buffer);
static unsigned parse_hex4(const unsigned char * const input);
static char *cJSON_strdup(const char * const string);
static cJSON_bool print_value(const cJSON * const item, print_buffer * const output_buffer);

static cJSON *cJSON_New_Item(void)
{
    cJSON *node = (cJSON*)calloc(1, sizeof(cJSON));
    if (node != NULL) {
        node->type = cJSON_Invalid;
    }
    return node;
}

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) global_ep;
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item)) {
        return NULL;
    }
    return item->valuestring;
}

CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item)) {
        return (double) NAN;
    }
    return item->valuedouble;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    return (cJSON_bool)((item == NULL) || ((item->type & 0xFF) == cJSON_Invalid));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_False));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_True));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    return (cJSON_bool)(cJSON_IsTrue(item) || cJSON_IsFalse(item));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_NULL));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_Number));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_String));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_Array));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_Object));
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    return (cJSON_bool)((item != NULL) && ((item->type & 0xFF) == cJSON_Raw));
}

static char *cJSON_strdup(const char * const string)
{
    size_t length = 0;
    char *copy = NULL;

    if (string == NULL) {
        return NULL;
    }

    length = strlen(string) + 1;
    copy = (char*)malloc(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, string, length);
    return copy;
}

/* Parse 4 hexadecimal digits (of a \uXXXX escape). */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++) {
        unsigned char c = input[i];
        if ((c >= '0') && (c <= '9')) {
            h += (unsigned int)c - '0';
        } else if ((c >= 'A') && (c <= 'F')) {
            h += (unsigned int)10 + c - 'A';
        } else if ((c >= 'a') && (c <= 'f')) {
            h += (unsigned int)10 + c - 'a';
        } else {
            return 0; /* invalid */
        }

        if (i < 3) {
            h = h << 4;
        }
    }

    return h;
}

/* Converts a UTF-16 literal (\uXXXX, optionally a surrogate pair) to UTF-8.
 * Returns the number of input bytes consumed, 0 on failure.
 * The output pointer is advanced by the number of bytes written. */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer,
                                           const unsigned char * const input_end,
                                           unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;
    unsigned int first_code = 0;
    const unsigned char *first_sequence = input_pointer;
    unsigned char utf8_length = 0;
    unsigned char utf8_position = 0;
    unsigned char sequence_length = 0;
    unsigned char first_byte_mark = 0;

    if ((input_end - first_sequence) < 6) {
        /* input ends unexpectedly */
        goto fail;
    }

    first_code = parse_hex4(first_sequence + 2);

    /* check that the code is valid */
    if ((first_code >= 0xDC00) && (first_code <= 0xDFFF)) {
        goto fail;
    }

    /* UTF16 surrogate pair */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF)) {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* \uXXXX\uXXXX */

        if ((input_end - second_sequence) < 6) {
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u')) {
            goto fail;
        }

        second_code = parse_hex4(second_sequence + 2);
        if ((second_code < 0xDC00) || (second_code > 0xDFFF)) {
            goto fail;
        }

        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    } else {
        sequence_length = 6; /* \uXXXX */
        codepoint = first_code;
    }

    /* encode as UTF-8 */
    if (codepoint < 0x80) {
        /* normal ascii, encoding 0xxxxxxx */
        utf8_length = 1;
    } else if (codepoint < 0x800) {
        /* two bytes, encoding 110xxxxx 10xxxxxx */
        utf8_length = 2;
        first_byte_mark = 0xC0;
    } else if (codepoint < 0x10000) {
        /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_length = 3;
        first_byte_mark = 0xE0;
    } else if (codepoint <= 0x10FFFF) {
        /* four bytes, encoding 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_length = 4;
        first_byte_mark = 0xF0;
    } else {
        goto fail;
    }

    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--) {
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* encode first byte */
    if (utf8_length > 1) {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    } else {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;
    return sequence_length;

fail:
    return 0;
}

/* Parse the input text into an unescaped string, and populate item. */
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
    unsigned char *output_pointer = NULL;
    unsigned char *output = NULL;

    /* not a string */
    if (buffer_at_offset(input_buffer)[0] != '\"') {
        goto fail;
    }

    {
        /* calculate approximate size of the output (overestimate) */
        size_t allocation_length = 0;
        size_t skipped_bytes = 0;
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"')) {
            if (input_end[0] == '\\') {
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length) {
                    /* incomplete escape sequence */
                    goto fail;
                }
                skipped_bytes++;
                input_end++;
            }
            input_end++;
        }
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"')) {
            goto fail; /* string ended unexpectedly */
        }

        allocation_length = (size_t)(input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        output = (unsigned char*)malloc(allocation_length + sizeof(""));
        if (output == NULL) {
            goto fail; /* allocation failure */
        }
    }

    output_pointer = output;
    /* loop through the string literal */
    while (input_pointer < input_end) {
        if (*input_pointer != '\\') {
            *output_pointer++ = *input_pointer++;
        } else {
            /* escape sequence */
            unsigned char sequence_length = 2;
            if ((input_end - input_pointer) < 1) {
                goto fail;
            }
            switch (input_pointer[1]) {
                case 'b': *output_pointer++ = '\b'; break;
                case 'f': *output_pointer++ = '\f'; break;
                case 'n': *output_pointer++ = '\n'; break;
                case 'r': *output_pointer++ = '\r'; break;
                case 't': *output_pointer++ = '\t'; break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;

                case 'u':
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length < 2) {
                        goto fail;
                    }
                    break;

                default:
                    goto fail;
            }
            input_pointer += sequence_length;
        }
    }

    /* zero terminate the output */
    *output_pointer = '\0';

    item->type = cJSON_String;
    item->valuestring = (char*)output;

    input_buffer->offset = (size_t)(input_end - input_buffer->content);
    input_buffer->offset++;

    return cJSON_true;

fail:
    if (output != NULL) {
        free(output);
    }
    if (input_pointer != NULL) {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }
    return cJSON_false;
}

/* Parse the input text to generate a number, and populate the result into item. */
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;
    char *number_c_string = NULL;
    size_t start = input_buffer->offset;
    size_t i = start;
    size_t length = 0;

    if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
        return cJSON_false;
    }

    /* scan the JSON number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
    if (can_access_at_index(input_buffer, i - start) && (buffer_at_offset(input_buffer)[i - start] == '-')) {
        i++;
    }
    if (cannot_access_at_index(input_buffer, i - start)) {
        return cJSON_false;
    }
    if (buffer_at_offset(input_buffer)[i - start] == '0') {
        i++;
    } else if ((buffer_at_offset(input_buffer)[i - start] >= '1') && (buffer_at_offset(input_buffer)[i - start] <= '9')) {
        while (can_access_at_index(input_buffer, i - start) &&
               isdigit((unsigned char)buffer_at_offset(input_buffer)[i - start])) {
            i++;
        }
    } else {
        return cJSON_false;
    }

    if (can_access_at_index(input_buffer, i - start) && (buffer_at_offset(input_buffer)[i - start] == '.')) {
        i++;
        if (cannot_access_at_index(input_buffer, i - start) ||
            !isdigit((unsigned char)buffer_at_offset(input_buffer)[i - start])) {
            return cJSON_false;
        }
        while (can_access_at_index(input_buffer, i - start) &&
               isdigit((unsigned char)buffer_at_offset(input_buffer)[i - start])) {
            i++;
        }
    }

    if (can_access_at_index(input_buffer, i - start) &&
        ((buffer_at_offset(input_buffer)[i - start] == 'e') || (buffer_at_offset(input_buffer)[i - start] == 'E'))) {
        i++;
        if (can_access_at_index(input_buffer, i - start) &&
            ((buffer_at_offset(input_buffer)[i - start] == '+') || (buffer_at_offset(input_buffer)[i - start] == '-'))) {
            i++;
        }
        if (cannot_access_at_index(input_buffer, i - start) ||
            !isdigit((unsigned char)buffer_at_offset(input_buffer)[i - start])) {
            return cJSON_false;
        }
        while (can_access_at_index(input_buffer, i - start) &&
               isdigit((unsigned char)buffer_at_offset(input_buffer)[i - start])) {
            i++;
        }
    }

    length = i - start;
    number_c_string = (char*)malloc(length + 1);
    if (number_c_string == NULL) {
        return cJSON_false;
    }
    memcpy(number_c_string, buffer_at_offset(input_buffer), length);
    number_c_string[length] = '\0';

    number = strtod(number_c_string, NULL);
    free(number_c_string);

    input_buffer->offset = i;

    item->valuedouble = number;

    /* use saturation in case of overflow */
    if (number >= INT_MAX) {
        item->valueint = INT_MAX;
    } else if (number <= (double)INT_MIN) {
        item->valueint = INT_MIN;
    } else {
        item->valueint = (int)number;
    }

    item->type = cJSON_Number;

    return cJSON_true;
}

/* Parse an object - text beginning with '{' */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;
    cJSON *current_item = NULL;

    if (buffer_at_offset(input_buffer)[0] != '{') {
        goto fail;
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}')) {
        /* empty object */
        goto success;
    }

    /* check if we skipped to the end of the buffer */
    if (cannot_access_at_index(input_buffer, 0)) {
        input_buffer->offset--;
        goto fail;
    }

    /* loop through the comma separated array elements */
    do {
        /* allocate next item */
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL) {
            goto fail; /* allocation failure */
        }

        /* attach next item to list */
        if (head == NULL) {
            current_item = head = new_item;
        } else {
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* parse the name of the child */
        buffer_skip_whitespace(input_buffer);
        if (!parse_string(current_item, input_buffer)) {
            goto fail;
        }

        /* swap valuestring and string, because we parsed the name */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':')) {
            goto fail; /* invalid object */
        }

        /* parse the value */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer)) {
            goto fail;
        }
        buffer_skip_whitespace(input_buffer);
        if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ',')) {
            /* consume the comma and parse the next entry */
            input_buffer->offset++;
            continue;
        }
        break;
    } while (1);

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}')) {
        goto fail; /* expected end of object */
    }

success:
    item->type = cJSON_Object;
    item->child = head;

    input_buffer->offset++;
    return cJSON_true;

fail:
    if (head != NULL) {
        cJSON_Delete(head);
    }
    return cJSON_false;
}

/* Parse an array - text beginning with '[' */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;
    cJSON *current_item = NULL;

    if (buffer_at_offset(input_buffer)[0] != '[') {
        goto fail;
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']')) {
        /* empty array */
        goto success;
    }

    /* check if we skipped to the end of the buffer */
    if (cannot_access_at_index(input_buffer, 0)) {
        input_buffer->offset--;
        goto fail;
    }

    /* loop through the comma separated array elements */
    do {
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL) {
            goto fail;
        }

        if (head == NULL) {
            current_item = head = new_item;
        } else {
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer)) {
            goto fail;
        }
        buffer_skip_whitespace(input_buffer);
        if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ',')) {
            /* consume the comma and parse the next element */
            input_buffer->offset++;
            continue;
        }
        break;
    } while (1);

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ']')) {
        goto fail;
    }

success:
    item->type = cJSON_Array;
    item->child = head;

    input_buffer->offset++;
    return cJSON_true;

fail:
    if (head != NULL) {
        cJSON_Delete(head);
    }
    return cJSON_false;
}

static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
        return cJSON_false;
    }

    if (cannot_access_at_index(input_buffer, 0)) {
        return cJSON_false; /* no more input */
    }

    /* parse the different types of values */
    /* null */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0)) {
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return cJSON_true;
    }
    /* false */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0)) {
        item->type = cJSON_False;
        input_buffer->offset += 5;
        return cJSON_true;
    }
    /* true */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0)) {
        item->type = cJSON_True;
        input_buffer->offset += 4;
        return cJSON_true;
    }
    /* string */
    if (buffer_at_offset(input_buffer)[0] == '\"') {
        return parse_string(item, input_buffer);
    }
    /* number */
    if ((buffer_at_offset(input_buffer)[0] == '-') ||
        ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))) {
        return parse_number(item, input_buffer);
    }
    /* array */
    if (buffer_at_offset(input_buffer)[0] == '[') {
        if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
            return cJSON_false;
        }
        input_buffer->depth++;
        {
            cJSON_bool ok = parse_array(item, input_buffer);
            input_buffer->depth--;
            return ok;
        }
    }
    /* object */
    if (buffer_at_offset(input_buffer)[0] == '{') {
        if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
            return cJSON_false;
        }
        input_buffer->depth++;
        {
            cJSON_bool ok = parse_object(item, input_buffer);
            input_buffer->depth--;
            return ok;
        }
    }

    return cJSON_false;
}

/* skip the UTF-8 BOM */
static cJSON_bool skip_utf8_bom(parse_buffer * const buffer)
{
    if ((can_read(buffer, 3)) && (buffer->content[0] == 0xEF) && (buffer->content[1] == 0xBB) && (buffer->content[2] == 0xBF)) {
        buffer->offset += 3;
        return cJSON_true;
    }
    return cJSON_false;
}

static void buffer_skip_whitespace(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL)) {
        return;
    }
    if (cannot_access_at_index(buffer, 0)) {
        return;
    }
    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 0x20)) {
        buffer->offset++;
    }
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length = 0;

    if (value == NULL) {
        return NULL;
    }

    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    parse_buffer buffer = { NULL, 0, 0, 0 };
    cJSON *item = NULL;

    /* reset error position */
    global_ep = NULL;

    if (value == NULL) {
        goto fail;
    }

    buffer.content = (const unsigned char*)value;
    buffer.length = buffer_length;
    buffer.offset = 0;
    buffer.depth = 0;

    item = cJSON_New_Item();
    if (item == NULL) {
        goto fail; /* memory failure */
    }

    skip_utf8_bom(&buffer);

    buffer_skip_whitespace(&buffer);
    if (!parse_value(item, &buffer)) {
        /* parse failure */
        goto fail;
    }

    /* if we require null-terminated JSON without appended garbage, skip and then check */
    buffer_skip_whitespace(&buffer);
    if (require_null_terminated && (buffer.offset != buffer.length)) {
        goto fail;
    }
    if (return_parse_end != NULL) {
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    return item;

fail:
    if (item != NULL) {
        cJSON_Delete(item);
    }
    if (value != NULL) {
        global_ep = value + buffer.offset;
    }
    if (return_parse_end != NULL) {
        *return_parse_end = global_ep;
    }
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, NULL, 0);
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, NULL, 0);
}

/* ---- printing ---- */

#define cjson_min(a, b) ((a < b) ? a : b)

static cJSON_bool printbuffer_ensure(print_buffer * const pbuf, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((pbuf == NULL) || (pbuf->buffer == NULL)) {
        return cJSON_false;
    }

    if ((pbuf->offset + needed) > pbuf->length) {
        if (pbuf->noalloc) {
            return cJSON_false;
        }
        /* double until it fits */
        newsize = (pbuf->length > 0) ? pbuf->length : 256;
        while (newsize < (pbuf->offset + needed + 1)) {
            newsize *= 2;
        }

        newbuffer = (unsigned char*)realloc(pbuf->buffer, newsize);
        if (newbuffer == NULL) {
            free(pbuf->buffer);
            pbuf->length = 0;
            pbuf->buffer = NULL;
            return cJSON_false;
        }
        pbuf->buffer = newbuffer;
        pbuf->length = newsize;
    }

    return cJSON_true;
}

static cJSON_bool printbuffer_append(print_buffer * const pbuf, const char * const data, size_t length)
{
    if (!printbuffer_ensure(pbuf, length + 1)) {
        return cJSON_false;
    }
    if (length > 0) {
        memcpy(pbuf->buffer + pbuf->offset, data, length);
    }
    pbuf->offset += length;
    pbuf->buffer[pbuf->offset] = '\0';
    return cJSON_true;
}

/* append one tab per indent level */
static cJSON_bool printbuffer_indent(print_buffer * const pbuf, size_t depth)
{
    size_t i = 0;
    for (i = 0; i < depth; i++) {
        if (!printbuffer_append(pbuf, "\t", 1)) {
            return cJSON_false;
        }
    }
    return cJSON_true;
}

/* Render a string (with quotes and escapes) to text. Non-ASCII bytes pass through verbatim. */
static cJSON_bool print_string_ptr(const unsigned char * const input, print_buffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;

    if (output_buffer == NULL) {
        return cJSON_false;
    }

    /* empty string */
    if (input == NULL) {
        return printbuffer_append(output_buffer, "\"\"", 2);
    }

    /* calculate the escaped length */
    input_pointer = input;
    output_length = 2; /* quotes */
    while (*input_pointer != '\0') {
        switch (*input_pointer) {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                output_length += 2;
                break;
            default:
                if (*input_pointer < 0x20) {
                    output_length += 6; /* \u00XX */
                } else {
                    output_length++;
                }
                break;
        }
        input_pointer++;
    }

    output = (unsigned char*)malloc(output_length + 1);
    if (output == NULL) {
        return cJSON_false;
    }
    output_pointer = output;
    *output_pointer++ = '\"';

    input_pointer = input;
    while (*input_pointer != '\0') {
        switch (*input_pointer) {
            case '\"': *output_pointer++ = '\\'; *output_pointer++ = '\"'; break;
            case '\\': *output_pointer++ = '\\'; *output_pointer++ = '\\'; break;
            case '\b': *output_pointer++ = '\\'; *output_pointer++ = 'b'; break;
            case '\f': *output_pointer++ = '\\'; *output_pointer++ = 'f'; break;
            case '\n': *output_pointer++ = '\\'; *output_pointer++ = 'n'; break;
            case '\r': *output_pointer++ = '\\'; *output_pointer++ = 'r'; break;
            case '\t': *output_pointer++ = '\\'; *output_pointer++ = 't'; break;
            default:
                if (*input_pointer < 0x20) {
                    sprintf((char*)output_pointer, "\\u%04x", *input_pointer);
                    output_pointer += 6;
                } else {
                    /* Non-ASCII bytes (UTF-8) are copied verbatim. */
                    *output_pointer++ = *input_pointer;
                }
                break;
        }
        input_pointer++;
    }
    *output_pointer++ = '\"';
    *output_pointer = '\0';

    {
        cJSON_bool ok = printbuffer_append(output_buffer, (const char*)output, output_pointer - output);
        free(output);
        return ok;
    }
}

static cJSON_bool print_string(const cJSON * const item, print_buffer * const pbuf)
{
    return print_string_ptr((const unsigned char*)item->valuestring, pbuf);
}

/* Render a number to text, using the shortest representation that round-trips. */
static cJSON_bool print_number(const cJSON * const item, print_buffer * const output_buffer)
{
    unsigned char number_buffer[26] = { 0 };
    double d = item->valuedouble;
    int i = 0;

    if (output_buffer == NULL) {
        return cJSON_false;
    }

    /* this checks for NaN and Infinity */
    if (isnan(d) || isinf(d)) {
        return printbuffer_append(output_buffer, "null", 4);
    }

    /* try 15 decimal places, then 16, then 17 for a shortest round-trip representation */
    for (i = 0; i < 3; i++) {
        int precision = 15 + i;
        sprintf((char*)number_buffer, "%1.*g", precision, d);
        if (strtod((const char*)number_buffer, NULL) == d) {
            break;
        }
    }

    return printbuffer_append(output_buffer, (const char*)number_buffer, strlen((const char*)number_buffer));
}

/* Render an array to text. */
static cJSON_bool print_array(const cJSON * const item, print_buffer * const output_buffer)
{
    cJSON *current_element = item->child;

    if (output_buffer == NULL) {
        return cJSON_false;
    }

    if (!printbuffer_append(output_buffer, "[", 1)) {
        return cJSON_false;
    }
    output_buffer->depth++;

    while (current_element != NULL) {
        if (!print_value(current_element, output_buffer)) {
            output_buffer->depth--;
            return cJSON_false;
        }
        if (current_element->next != NULL) {
            if (!printbuffer_append(output_buffer, ",", 1)) {
                output_buffer->depth--;
                return cJSON_false;
            }
            if (output_buffer->format) {
                if (!printbuffer_append(output_buffer, " ", 1)) {
                    output_buffer->depth--;
                    return cJSON_false;
                }
            }
        }
        current_element = current_element->next;
    }

    output_buffer->depth--;

    if (output_buffer->format) {
        if (!printbuffer_append(output_buffer, "\n", 1) || !printbuffer_indent(output_buffer, output_buffer->depth)) {
            return cJSON_false;
        }
    }

    return printbuffer_append(output_buffer, "]", 1);
}

/* Render an object to text. */
static cJSON_bool print_object(const cJSON * const item, print_buffer * const output_buffer)
{
    cJSON *current_item = item->child;

    if (output_buffer == NULL) {
        return cJSON_false;
    }

    if (!printbuffer_append(output_buffer, "{", 1)) {
        return cJSON_false;
    }
    if (output_buffer->format && (current_item != NULL)) {
        if (!printbuffer_append(output_buffer, "\n", 1) ||
            !printbuffer_indent(output_buffer, output_buffer->depth + 1)) {
            return cJSON_false;
        }
    }
    output_buffer->depth++;

    while (current_item != NULL) {
        if (current_item->string == NULL) {
            /* printing a keyless entry is invalid */
            output_buffer->depth--;
            return cJSON_false;
        }
        if (!print_string_ptr((const unsigned char*)current_item->string, output_buffer)) {
            output_buffer->depth--;
            return cJSON_false;
        }
        if (output_buffer->format) {
            if (!printbuffer_append(output_buffer, ": ", 2)) {
                output_buffer->depth--;
                return cJSON_false;
            }
        } else {
            if (!printbuffer_append(output_buffer, ":", 1)) {
                output_buffer->depth--;
                return cJSON_false;
            }
        }

        if (!print_value(current_item, output_buffer)) {
            output_buffer->depth--;
            return cJSON_false;
        }

        if (current_item->next != NULL) {
            if (!printbuffer_append(output_buffer, ",", 1)) {
                output_buffer->depth--;
                return cJSON_false;
            }
            if (output_buffer->format) {
                if (!printbuffer_append(output_buffer, "\n", 1) ||
                    !printbuffer_indent(output_buffer, output_buffer->depth)) {
                    output_buffer->depth--;
                    return cJSON_false;
                }
            }
        }
        current_item = current_item->next;
    }

    output_buffer->depth--;

    if (output_buffer->format && (item->child != NULL)) {
        if (!printbuffer_append(output_buffer, "\n", 1) || !printbuffer_indent(output_buffer, output_buffer->depth)) {
            return cJSON_false;
        }
    }

    return printbuffer_append(output_buffer, "}", 1);
}

/* Render a value to text. */
static cJSON_bool print_value(const cJSON * const item, print_buffer * const output_buffer)
{
    if ((item == NULL) || (output_buffer == NULL)) {
        return cJSON_false;
    }

    switch (item->type & 0xFF) {
        case cJSON_NULL:
            return printbuffer_append(output_buffer, "null", 4);

        case cJSON_False:
            return printbuffer_append(output_buffer, "false", 5);

        case cJSON_True:
            return printbuffer_append(output_buffer, "true", 4);

        case cJSON_Number:
            return print_number(item, output_buffer);

        case cJSON_Raw:
            if (item->valuestring == NULL) {
                return cJSON_false;
            }
            return printbuffer_append(output_buffer, item->valuestring, strlen(item->valuestring));

        case cJSON_String:
            return print_string(item, output_buffer);

        case cJSON_Array:
            return print_array(item, output_buffer);

        case cJSON_Object:
            return print_object(item, output_buffer);

        default:
            return cJSON_false;
    }
}

static char *cJSON_print_internal(const cJSON *item, cJSON_bool fmt)
{
    static const int prebuffer = 256;
    print_buffer p = { NULL, 0, 0, 0, 0, 0 };

    p.buffer = (unsigned char*)malloc((size_t)prebuffer);
    if (p.buffer == NULL) {
        return NULL;
    }
    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = cJSON_false;
    p.format = fmt;
    p.depth = 0;

    if (!print_value(item, &p)) {
        if (p.buffer != NULL) {
            free(p.buffer);
        }
        return NULL;
    }

    return (char*)p.buffer;
}

CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    return cJSON_print_internal(item, cJSON_true);
}

CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    return cJSON_print_internal(item, cJSON_false);
}

CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    print_buffer p = { NULL, 0, 0, 0, 0, 0 };

    if (prebuffer < 0) {
        return NULL;
    }

    p.buffer = (unsigned char*)malloc((size_t)prebuffer);
    if (p.buffer == NULL) {
        return NULL;
    }
    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = cJSON_false;
    p.format = fmt;
    p.depth = 0;

    if (!print_value(item, &p)) {
        if (p.buffer != NULL) {
            free(p.buffer);
        }
        return NULL;
    }

    return (char*)p.buffer;
}

CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    print_buffer p = { NULL, 0, 0, 0, 0, 0 };

    if ((length < 0) || (buffer == NULL)) {
        return cJSON_false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = cJSON_true;
    p.format = format;
    p.depth = 0;

    return print_value(item, &p);
}

CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    unsigned char *into = (unsigned char*)json;
    unsigned char *from = (unsigned char*)json;

    if (json == NULL) {
        return;
    }

    while (*from) {
        if ((*from == ' ') || (*from == '\t') || (*from == '\r') || (*from == '\n')) {
            from++;
            continue;
        }

        if ((*from == '/') && (from[1] == '/')) {
            while (*from && (*from != '\n')) {
                from++;
            }
            continue;
        }

        if ((*from == '/') && (from[1] == '*')) {
            from += 2;
            while (*from && !((*from == '*') && (from[1] == '/'))) {
                from++;
            }
            if (*from) {
                from += 2;
            }
            continue;
        }

        if (*from == '\"') {
            /* copy the string verbatim */
            *into++ = *from++;
            while (*from && (*from != '\"')) {
                if (*from == '\\') {
                    *into++ = *from++;
                }
                if (*from) {
                    *into++ = *from++;
                }
            }
            if (*from) {
                *into++ = *from++;
            }
            continue;
        }

        *into++ = *from++;
    }

    *into = '\0';
}

/* ---- constructors ---- */

CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_NULL;
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_True;
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_False;
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = boolean ? cJSON_True : cJSON_False;
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* use saturation in case of overflow */
        if (num >= INT_MAX) {
            item->valueint = INT_MAX;
        } else if (num <= (double)INT_MIN) {
            item->valueint = INT_MIN;
        } else {
            item->valueint = (int)num;
        }
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_String;
        item->valuestring = cJSON_strdup(string);
        if ((item->valuestring == NULL) && (string != NULL)) {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_Raw;
        item->valuestring = cJSON_strdup(raw);
        if ((item->valuestring == NULL) && (raw != NULL)) {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_Array;
    }
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    if (item != NULL) {
        item->type = cJSON_Object;
    }
    return item;
}

/* ---- adding items ---- */

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL)) {
        return cJSON_false;
    }

    child = array->child;

    if (child == NULL) {
        /* list is empty, start new one */
        array->child = item;
        item->prev = item->next = NULL;
    } else {
        /* append to the end */
        while (child->next != NULL) {
            child = child->next;
        }
        child->next = item;
        item->prev = child;
        item->next = NULL;
    }

    return cJSON_true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((item == NULL) || (object == NULL) || (string == NULL)) {
        return cJSON_false;
    }

    /* allocate the key */
    if (!(item->type & cJSON_StringIsConst)) {
        char *key = cJSON_strdup(string);
        if (key == NULL) {
            return cJSON_false;
        }
        item->string = key;
    } else {
        item->string = (char*)string;
    }

    return cJSON_AddItemToArray(object, item);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    if ((item == NULL) || (object == NULL) || (string == NULL)) {
        return cJSON_false;
    }
    if (!(item->type & cJSON_StringIsConst)) {
        item->type |= cJSON_StringIsConst;
    }
    item->string = (char*)string;
    return cJSON_AddItemToArray(object, item);
}

static void *cast_away_const(const void *pointer)
{
    return (void*)(size_t)pointer;
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    cJSON *ref = cJSON_New_Item();
    if ((ref == NULL) || (array == NULL) || (item == NULL)) {
        cJSON_Delete(ref);
        return cJSON_false;
    }
    ref->type = (cJSON_Invalid | cJSON_IsReference);
    ref->child = (cJSON*)cast_away_const(item);
    return cJSON_AddItemToArray(array, ref);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    cJSON *ref = cJSON_New_Item();
    if ((ref == NULL) || (object == NULL) || (item == NULL) || (string == NULL)) {
        cJSON_Delete(ref);
        return cJSON_false;
    }
    ref->type = (cJSON_Invalid | cJSON_IsReference);
    ref->child = (cJSON*)cast_away_const(item);
    return cJSON_AddItemToObject(object, string, ref);
}

CJSON_PUBLIC(cJSON *) cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *null_item = cJSON_CreateNull();
    if (cJSON_AddItemToObject(object, name, null_item)) {
        return null_item;
    }
    cJSON_Delete(null_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (cJSON_AddItemToObject(object, name, true_item)) {
        return true_item;
    }
    cJSON_Delete(true_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (cJSON_AddItemToObject(object, name, false_item)) {
        return false_item;
    }
    cJSON_Delete(false_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (cJSON_AddItemToObject(object, name, bool_item)) {
        return bool_item;
    }
    cJSON_Delete(bool_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (cJSON_AddItemToObject(object, name, number_item)) {
        return number_item;
    }
    cJSON_Delete(number_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (cJSON_AddItemToObject(object, name, string_item)) {
        return string_item;
    }
    cJSON_Delete(string_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (cJSON_AddItemToObject(object, name, raw_item)) {
        return raw_item;
    }
    cJSON_Delete(raw_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (cJSON_AddItemToObject(object, name, object_item)) {
        return object_item;
    }
    cJSON_Delete(object_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array_item = cJSON_CreateArray();
    if (cJSON_AddItemToObject(object, name, array_item)) {
        return array_item;
    }
    cJSON_Delete(array_item);
    return NULL;
}

CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (number >= INT_MAX) {
        object->valueint = INT_MAX;
    } else if (number <= (double)INT_MIN) {
        object->valueint = INT_MIN;
    } else {
        object->valueint = (int)number;
    }
    return object->valuedouble = number;
}

CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;

    /* if object's type is not cJSON_String or is cJSON_IsReference, it is invalid to set its valuestring */
    if (!cJSON_IsString(object) || (object->type & cJSON_IsReference)) {
        return NULL;
    }
    if (strlen(valuestring) > strlen(object->valuestring)) {
        copy = cJSON_strdup(valuestring);
        if (copy == NULL) {
            return NULL;
        }
        free(object->valuestring);
        object->valuestring = copy;
    } else {
        strcpy(object->valuestring, valuestring);
    }

    return object->valuestring;
}

/* ---- accessors ---- */

CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t i = 0;

    if (array == NULL) {
        return 0;
    }

    child = array->child;
    while (child != NULL) {
        i++;
        child = child->next;
    }

    return (int)i;
}

static cJSON *get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL) {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0)) {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0) {
        return NULL;
    }
    return get_array_item(array, (size_t)index);
}

static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL)) {
        return NULL;
    }

    current_element = object->child;
    while (current_element != NULL) {
        if (current_element->string != NULL) {
            if (case_sensitive
                ? (strcmp(current_element->string, name) == 0)
                : (strcasecmp(current_element->string, name) == 0)) {
                return current_element;
            }
        }
        current_element = current_element->next;
    }

    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, cJSON_false);
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, cJSON_true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return (cJSON_bool)(get_object_item(object, string, cJSON_true) != NULL);
}

/* ---- detach / delete / insert / replace ---- */

CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL)) {
        return NULL;
    }
    if ((item->prev == NULL) && (item->next == NULL) && (parent->child != item)) {
        /* item is not part of parent's list */
        return NULL;
    }

    if (item->prev != NULL) {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL) {
        /* not the last element */
        item->next->prev = item->prev;
    }
    if (item == parent->child) {
        /* first element */
        parent->child = item->next;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0) {
        return NULL;
    }
    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);
    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);
    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if ((which < 0) || (newitem == NULL)) {
        return cJSON_false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL) {
        return cJSON_AddItemToArray(array, newitem);
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child) {
        array->child = newitem;
    } else {
        if (newitem->prev != NULL) {
            newitem->prev->next = newitem;
        }
    }

    return cJSON_true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (replacement == NULL) || (item == NULL)) {
        return cJSON_false;
    }

    if (replacement == item) {
        return cJSON_true;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL) {
        replacement->next->prev = replacement;
    }
    if (replacement->prev != NULL) {
        replacement->prev->next = replacement;
    }
    if (parent->child == item) {
        parent->child = replacement;
    }

    item->next = NULL;
    item->prev = NULL;

    return cJSON_true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0) {
        return cJSON_false;
    }
    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    cJSON *target = case_sensitive
        ? cJSON_GetObjectItemCaseSensitive(object, string)
        : cJSON_GetObjectItem(object, string);

    if ((replacement == NULL) || (target == NULL) || (string == NULL)) {
        return cJSON_false;
    }
    if (cJSON_ReplaceItemViaPointer(object, target, replacement) == cJSON_false) {
        return cJSON_false;
    }
    /* set the new key */
    if (!(replacement->type & cJSON_StringIsConst)) {
        char *key = cJSON_strdup(string);
        if (key == NULL) {
            return cJSON_false;
        }
        if (replacement->string != NULL) {
            free(replacement->string);
        }
        replacement->string = key;
    } else {
        replacement->string = (char*)string;
    }
    return cJSON_true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, cJSON_false);
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, cJSON_true);
}

/* ---- typed array constructors ---- */

static cJSON *create_number_array_from(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();
    for (i = 0; (a != NULL) && (i < (size_t)count); i++) {
        n = cJSON_CreateNumber(numbers[i]);
        if (n == NULL) {
            cJSON_Delete(a);
            return NULL;
        }
        if (i == 0) {
            a->child = n;
        } else {
            p->next = n;
            n->prev = p;
        }
        p = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    double *values = NULL;
    cJSON *result = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    values = (double*)malloc(sizeof(double) * (size_t)count);
    if (values == NULL) {
        return NULL;
    }
    for (i = 0; i < (size_t)count; i++) {
        values[i] = (double)numbers[i];
    }
    result = create_number_array_from(values, count);
    free(values);
    return result;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    double *values = NULL;
    cJSON *result = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    values = (double*)malloc(sizeof(double) * (size_t)count);
    if (values == NULL) {
        return NULL;
    }
    for (i = 0; i < (size_t)count; i++) {
        values[i] = (double)numbers[i];
    }
    result = create_number_array_from(values, count);
    free(values);
    return result;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    return create_number_array_from(numbers, count);
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();
    for (i = 0; (a != NULL) && (i < (size_t)count); i++) {
        n = cJSON_CreateString(strings[i]);
        if (n == NULL) {
            cJSON_Delete(a);
            return NULL;
        }
        if (i == 0) {
            a->child = n;
        } else {
            p->next = n;
            n->prev = p;
        }
        p = n;
    }

    return a;
}

/* ---- duplicate / compare ---- */

CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    cJSON *newitem = NULL;
    cJSON *child = NULL;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    /* bail on bad ptr */
    if (item == NULL) {
        return NULL;
    }
    /* create new item */
    newitem = cJSON_New_Item();
    if (newitem == NULL) {
        return NULL;
    }
    /* copy over all vars */
    newitem->type = item->type & (cJSON_bool)~(cJSON_IsReference | cJSON_StringIsConst);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring != NULL) {
        newitem->valuestring = cJSON_strdup(item->valuestring);
        if (newitem->valuestring == NULL) {
            cJSON_Delete(newitem);
            return NULL;
        }
    }
    if (item->string != NULL) {
        newitem->string = cJSON_strdup(item->string);
        if (newitem->string == NULL) {
            cJSON_Delete(newitem);
            return NULL;
        }
    }
    if (item->type & cJSON_IsReference) {
        /* references are not duplicated */
        return newitem;
    }
    /* Walk the ->next chain for the child. */
    child = item->child;
    while (child != NULL) {
        newchild = cJSON_Duplicate(child, recurse);
        if (newchild == NULL) {
            cJSON_Delete(newitem);
            return NULL;
        }
        if (next != NULL) {
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        } else {
            newitem->child = next = newchild;
        }
        child = child->next;
    }

    return newitem;
}

CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF))) {
        return cJSON_false;
    }

    /* same item pointer */
    if (a == b) {
        return cJSON_true;
    }

    switch (a->type & 0xFF) {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Invalid:
            return cJSON_true;

        case cJSON_Number:
            if (isnan(a->valuedouble) || isnan(b->valuedouble)) {
                return cJSON_false;
            }
            return (cJSON_bool)(a->valuedouble == b->valuedouble);

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL)) {
                return cJSON_false;
            }
            return (cJSON_bool)(strcmp(a->valuestring, b->valuestring) == 0);

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;
            for (; (a_element != NULL) && (b_element != NULL);) {
                if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
                    return cJSON_false;
                }
                a_element = a_element->next;
                b_element = b_element->next;
            }
            return (cJSON_bool)((a_element == NULL) && (b_element == NULL));
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a) {
                b_element = case_sensitive
                    ? cJSON_GetObjectItemCaseSensitive(b, a_element->string)
                    : cJSON_GetObjectItem(b, a_element->string);
                if (b_element == NULL) {
                    return cJSON_false;
                }
                if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
                    return cJSON_false;
                }
            }
            cJSON_ArrayForEach(a_element, b) {
                b_element = case_sensitive
                    ? cJSON_GetObjectItemCaseSensitive(a, a_element->string)
                    : cJSON_GetObjectItem(a, a_element->string);
                if (b_element == NULL) {
                    return cJSON_false;
                }
            }
            return cJSON_true;
        }

        default:
            return cJSON_false;
    }
}

/* ---- references ---- */

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item == NULL) {
        return NULL;
    }
    item->type = cJSON_String | cJSON_IsReference;
    item->valuestring = (char*)cast_away_const(string);
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item();
    if (item == NULL) {
        return NULL;
    }
    item->type = cJSON_Object | cJSON_IsReference;
    item->child = (cJSON*)cast_away_const(child);
    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item();
    if (item == NULL) {
        return NULL;
    }
    item->type = cJSON_Array | cJSON_IsReference;
    item->child = (cJSON*)cast_away_const(child);
    return item;
}

/* ---- memory hooks ---- */

static void *internal_malloc(size_t size)
{
    return malloc(size);
}

static void internal_free(void *pointer)
{
    free(pointer);
}

static void *(*global_allocate)(size_t size) = internal_malloc;
static void (*global_deallocate)(void *pointer) = internal_free;

CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL) {
        /* reset hooks */
        global_allocate = internal_malloc;
        global_deallocate = internal_free;
        return;
    }

    if (hooks->malloc_fn != NULL) {
        global_allocate = hooks->malloc_fn;
    } else {
        global_allocate = internal_malloc;
    }
    if (hooks->free_fn != NULL) {
        global_deallocate = hooks->free_fn;
    } else {
        global_deallocate = internal_free;
    }
}

CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_deallocate(object);
}

CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    return "1.7.19";
}

/* Delete a cJSON structure. (kept last so the type is fully defined above) */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL) {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL)) {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_StringIsConst)) {
            if (item->valuestring != NULL) {
                free(item->valuestring);
            }
            if (item->string != NULL) {
                free(item->string);
            }
        }
        free(item);
        item = next;
    }
}
