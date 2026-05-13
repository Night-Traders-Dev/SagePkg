#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SageValue SageValue;
typedef struct SageGcHeader SageGcHeader;
typedef struct SageGcFrame SageGcFrame;

typedef struct {
    int count;
    int capacity;
    SageValue* elements;
} SageArray;

typedef struct {
    char** keys;
    SageValue* values;
    int count;
    int capacity;
} SageDict;

typedef struct {
    SageValue* elements;
    int count;
} SageTuple;

typedef enum {
    SAGE_TAG_NIL,
    SAGE_TAG_NUMBER,
    SAGE_TAG_BOOL,
    SAGE_TAG_STRING,
    SAGE_TAG_ARRAY,
    SAGE_TAG_DICT,
    SAGE_TAG_TUPLE
} SageTag;

struct SageValue {
    SageTag type;
    union {
        double number;
        int boolean;
        const char* string;
        SageArray* array;
        SageDict* dict;
        SageTuple* tuple;
    } as;
};

typedef struct {
    int defined;
    SageValue value;
} SageSlot;

typedef enum {
    SAGE_GC_STRING,
    SAGE_GC_ARRAY,
    SAGE_GC_DICT,
    SAGE_GC_TUPLE
} SageGcKind;

struct SageGcHeader {
    unsigned char marked;
    unsigned char kind;
    size_t size;
    SageGcHeader* next;
};

struct SageGcFrame {
    SageGcFrame* prev;
    SageSlot** slots;
    int slot_count;
};

typedef struct {
    SageGcHeader* objects;
    SageGcFrame* frames;
    int object_count;
    int collections;
    int pin_count;
    unsigned long bytes_allocated;
    unsigned long bytes_freed;
    unsigned long next_gc_bytes;
    int next_gc_objects;
    int enabled;
} SageGcState;

#define SAGE_GC_MIN_TRIGGER_BYTES 65536UL
#define SAGE_GC_MIN_TRIGGER_OBJECTS 128
static SageGcState sage_gc = {NULL, NULL, 0, 0, 0, 0, 0, SAGE_GC_MIN_TRIGGER_BYTES, SAGE_GC_MIN_TRIGGER_OBJECTS, 1};

/* Exception handling via setjmp/longjmp */
#define SAGE_MAX_TRY_DEPTH 64
static jmp_buf sage_try_stack[SAGE_MAX_TRY_DEPTH];
static SageValue sage_exception_value;
static int sage_try_depth = 0;

static void sage_fail(const char* message) {
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}

static unsigned long sage_gc_live_bytes(void) {
    return sage_gc.bytes_allocated - sage_gc.bytes_freed;
}

static void sage_gc_recompute_thresholds(unsigned long reclaimed_bytes, int reclaimed_objects) {
    unsigned long live_bytes = sage_gc_live_bytes();
    int live_objects = sage_gc.object_count;
    unsigned long byte_padding = live_bytes / 2;
    int object_padding = live_objects / 2;
    if (byte_padding < (SAGE_GC_MIN_TRIGGER_BYTES / 2)) byte_padding = SAGE_GC_MIN_TRIGGER_BYTES / 2;
    if (object_padding < (SAGE_GC_MIN_TRIGGER_OBJECTS / 2)) object_padding = SAGE_GC_MIN_TRIGGER_OBJECTS / 2;
    if (reclaimed_bytes <= live_bytes / 8) {
        byte_padding /= 2;
        if (byte_padding < (SAGE_GC_MIN_TRIGGER_BYTES / 2)) byte_padding = SAGE_GC_MIN_TRIGGER_BYTES / 2;
    } else if (reclaimed_bytes >= live_bytes) {
        byte_padding *= 2;
    }
    if (reclaimed_objects <= live_objects / 8) {
        object_padding /= 2;
        if (object_padding < (SAGE_GC_MIN_TRIGGER_OBJECTS / 2)) object_padding = SAGE_GC_MIN_TRIGGER_OBJECTS / 2;
    } else if (reclaimed_objects >= live_objects) {
        object_padding *= 2;
    }
    sage_gc.next_gc_bytes = live_bytes + byte_padding;
    if (sage_gc.next_gc_bytes < SAGE_GC_MIN_TRIGGER_BYTES) sage_gc.next_gc_bytes = SAGE_GC_MIN_TRIGGER_BYTES;
    sage_gc.next_gc_objects = live_objects + object_padding;
    if (sage_gc.next_gc_objects < SAGE_GC_MIN_TRIGGER_OBJECTS) sage_gc.next_gc_objects = SAGE_GC_MIN_TRIGGER_OBJECTS;
}

static int sage_gc_try_mark(void* object) {
    if (object == NULL) return 0;
    SageGcHeader* header = ((SageGcHeader*)object) - 1;
    if (header->marked) return 0;
    header->marked = 1;
    return 1;
}

static void sage_gc_mark_value(SageValue value);

static void sage_gc_mark_roots(void) {
    for (SageGcFrame* frame = sage_gc.frames; frame != NULL; frame = frame->prev) {
        if (frame->slots == NULL) continue;
        for (int i = 0; i < frame->slot_count; i++) {
            if (frame->slots[i] != NULL && frame->slots[i]->defined) {
                sage_gc_mark_value(frame->slots[i]->value);
            }
        }
    }
    if (sage_try_depth > 0) sage_gc_mark_value(sage_exception_value);
}

static size_t sage_gc_release_object(SageGcHeader* header) {
    void* object = (void*)(header + 1);
    size_t freed = sizeof(SageGcHeader) + header->size;
    switch ((SageGcKind)header->kind) {
        case SAGE_GC_STRING:
            break;
        case SAGE_GC_ARRAY: {
            SageArray* array = (SageArray*)object;
            freed += sizeof(SageValue) * (size_t)array->capacity;
            free(array->elements);
            break;
        }
        case SAGE_GC_DICT: {
            SageDict* dict = (SageDict*)object;
            freed += sizeof(char*) * (size_t)dict->capacity;
            freed += sizeof(SageValue) * (size_t)dict->capacity;
            for (int i = 0; i < dict->count; i++) {
                if (dict->keys[i] != NULL) {
                    freed += strlen(dict->keys[i]) + 1;
                    free(dict->keys[i]);
                }
            }
            free(dict->keys);
            free(dict->values);
            break;
        }
        case SAGE_GC_TUPLE: {
            SageTuple* tuple = (SageTuple*)object;
            freed += sizeof(SageValue) * (size_t)tuple->count;
            free(tuple->elements);
            break;
        }
    }
    return freed;
}

static void sage_gc_collect(void) {
    if (!sage_gc.enabled) return;
    unsigned long before_bytes = sage_gc_live_bytes();
    int before_objects = sage_gc.object_count;
    sage_gc_mark_roots();
    SageGcHeader** current = &sage_gc.objects;
    while (*current != NULL) {
        SageGcHeader* header = *current;
        if (!header->marked) {
            *current = header->next;
            sage_gc.object_count--;
            sage_gc.bytes_freed += sage_gc_release_object(header);
            free(header);
        } else {
            header->marked = 0;
            current = &header->next;
        }
    }
    sage_gc.collections++;
    sage_gc_recompute_thresholds(before_bytes - sage_gc_live_bytes(), before_objects - sage_gc.object_count);
}

static int sage_gc_should_collect(size_t incoming_size) {
    if (!sage_gc.enabled || sage_gc.pin_count > 0) return 0;
    if ((sage_gc.object_count + 1) >= sage_gc.next_gc_objects) return 1;
    return sage_gc_live_bytes() + (unsigned long)sizeof(SageGcHeader) + (unsigned long)incoming_size >= sage_gc.next_gc_bytes;
}

static void* sage_gc_alloc(SageGcKind kind, size_t size) {
    if (sage_gc.frames != NULL && sage_gc_should_collect(size)) sage_gc_collect();
    size_t total = sizeof(SageGcHeader) + size;
    SageGcHeader* header = (SageGcHeader*)malloc(total);
    if (header == NULL) sage_fail("Runtime Error: out of memory");
    header->marked = 0;
    header->kind = (unsigned char)kind;
    header->size = size;
    header->next = sage_gc.objects;
    sage_gc.objects = header;
    sage_gc.object_count++;
    sage_gc.bytes_allocated += (unsigned long)total;
    return (void*)(header + 1);
}

static void sage_gc_push_frame(SageGcFrame* frame, SageSlot** slots, int slot_count) {
    frame->prev = sage_gc.frames;
    frame->slots = slots;
    frame->slot_count = slot_count;
    sage_gc.frames = frame;
}

static void sage_gc_pop_frame(SageGcFrame* frame) {
    if (sage_gc.frames == frame) sage_gc.frames = frame->prev;
}

static void sage_gc_pin(void) { sage_gc.pin_count++; }
static void sage_gc_unpin(void) { if (sage_gc.pin_count > 0) sage_gc.pin_count--; }

static SageValue sage_gc_return(SageGcFrame* frame, SageValue value) {
    sage_gc_pop_frame(frame);
    return value;
}

static void sage_gc_shutdown(void) {
    SageGcHeader* object = sage_gc.objects;
    while (object != NULL) {
        SageGcHeader* next = object->next;
        sage_gc.bytes_freed += sage_gc_release_object(object);
        free(object);
        object = next;
    }
    sage_gc.objects = NULL;
    sage_gc.object_count = 0;
}

static void sage_gc_mark_value(SageValue value) {
    switch (value.type) {
        case SAGE_TAG_STRING:
            (void)sage_gc_try_mark((void*)value.as.string);
            return;
        case SAGE_TAG_ARRAY:
            if (sage_gc_try_mark(value.as.array)) {
                for (int i = 0; i < value.as.array->count; i++) sage_gc_mark_value(value.as.array->elements[i]);
            }
            return;
        case SAGE_TAG_DICT:
            if (sage_gc_try_mark(value.as.dict)) {
                for (int i = 0; i < value.as.dict->count; i++) sage_gc_mark_value(value.as.dict->values[i]);
            }
            return;
        case SAGE_TAG_TUPLE:
            if (sage_gc_try_mark(value.as.tuple)) {
                for (int i = 0; i < value.as.tuple->count; i++) sage_gc_mark_value(value.as.tuple->elements[i]);
            }
            return;
        default:
            return;
    }
}

static char* sage_dup_string(const char* text) {
    size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) sage_fail("Runtime Error: out of memory");
    memcpy(copy, text, len + 1);
    return copy;
}

static char* sage_gc_copy_string(const char* text) {
    size_t len = strlen(text);
    char* copy = (char*)sage_gc_alloc(SAGE_GC_STRING, len + 1);
    memcpy(copy, text, len + 1);
    return copy;
}

static SageArray* sage_new_array(void) {
    SageArray* array = (SageArray*)sage_gc_alloc(SAGE_GC_ARRAY, sizeof(SageArray));
    array->count = 0;
    array->capacity = 0;
    array->elements = NULL;
    return array;
}

static SageValue sage_nil(void) { SageValue v; v.type = SAGE_TAG_NIL; v.as.number = 0; return v; }
static SageValue sage_number(double value) { SageValue v; v.type = SAGE_TAG_NUMBER; v.as.number = value; return v; }
static SageValue sage_bool(int value) { SageValue v; v.type = SAGE_TAG_BOOL; v.as.boolean = value ? 1 : 0; return v; }
static SageValue sage_string(const char* value) { SageValue v; v.type = SAGE_TAG_STRING; v.as.string = sage_gc_copy_string(value == NULL ? "" : value); return v; }
static SageValue sage_string_take(char* value) { SageValue v = sage_string(value == NULL ? "" : value); free(value); return v; }
static SageValue sage_array(void) { SageValue v; v.type = SAGE_TAG_ARRAY; v.as.array = sage_new_array(); return v; }
static SageSlot sage_slot_undefined(void) { SageSlot slot; slot.defined = 0; slot.value = sage_nil(); return slot; }

static SageValue sage_make_dict(void) {
    SageDict* dict = (SageDict*)sage_gc_alloc(SAGE_GC_DICT, sizeof(SageDict));
    dict->keys = NULL;
    dict->values = NULL;
    dict->count = 0;
    dict->capacity = 0;
    SageValue v; v.type = SAGE_TAG_DICT; v.as.dict = dict;
    return v;
}

static void sage_dict_set(SageDict* dict, const char* key, SageValue value) {
    for (int i = 0; i < dict->count; i++) {
        if (strcmp(dict->keys[i], key) == 0) {
            dict->values[i] = value;
            return;
        }
    }
    if (dict->count >= dict->capacity) {
        int cap = dict->capacity == 0 ? 4 : dict->capacity * 2;
        dict->keys = (char**)realloc(dict->keys, sizeof(char*) * (size_t)cap);
        dict->values = (SageValue*)realloc(dict->values, sizeof(SageValue) * (size_t)cap);
        if (dict->keys == NULL || dict->values == NULL) sage_fail("Runtime Error: out of memory");
        dict->capacity = cap;
    }
    dict->keys[dict->count] = sage_dup_string(key);
    dict->values[dict->count] = value;
    dict->count++;
}

static SageValue sage_make_dict_from_entries(int count, const char** keys, const SageValue* values) {
    sage_gc_pin();
    SageValue dict = sage_make_dict();
    for (int i = 0; i < count; i++) {
        sage_dict_set(dict.as.dict, keys[i], values[i]);
    }
    sage_gc_unpin();
    return dict;
}

static SageValue sage_dict_get(SageDict* dict, const char* key) {
    for (int i = 0; i < dict->count; i++) {
        if (strcmp(dict->keys[i], key) == 0) return dict->values[i];
    }
    return sage_nil();
}

static SageValue sage_make_tuple(int count, const SageValue* values) {
    sage_gc_pin();
    SageTuple* tuple = (SageTuple*)sage_gc_alloc(SAGE_GC_TUPLE, sizeof(SageTuple));
    tuple->count = count;
    tuple->elements = (SageValue*)malloc(sizeof(SageValue) * (size_t)count);
    if (tuple->elements == NULL && count > 0) sage_fail("Runtime Error: out of memory");
    for (int i = 0; i < count; i++) tuple->elements[i] = values[i];
    SageValue v; v.type = SAGE_TAG_TUPLE; v.as.tuple = tuple;
    sage_gc_unpin();
    return v;
}

static void sage_raise(SageValue value) {
    if (sage_try_depth > 0) {
        sage_exception_value = value;
        longjmp(sage_try_stack[sage_try_depth - 1], 1);
    }
    fputs("Unhandled exception: ", stderr);
    if (value.type == SAGE_TAG_STRING) fputs(value.as.string, stderr);
    else fputs("(unknown)", stderr);
    fputc('\n', stderr);
    exit(1);
}

static void sage_array_reserve(SageArray* array, int needed) {
    if (array->capacity >= needed) return;
    int capacity = array->capacity == 0 ? 4 : array->capacity;
    while (capacity < needed) capacity *= 2;
    SageValue* elements = (SageValue*)realloc(array->elements, sizeof(SageValue) * (size_t)capacity);
    if (elements == NULL) sage_fail("Runtime Error: out of memory");
    array->elements = elements;
    array->capacity = capacity;
}

static void sage_array_push_raw(SageArray* array, SageValue value) {
    sage_array_reserve(array, array->count + 1);
    array->elements[array->count++] = value;
}

static SageValue sage_make_array(int count, const SageValue* values) {
    sage_gc_pin();
    SageValue array = sage_array();
    for (int i = 0; i < count; i++) {
        sage_array_push_raw(array.as.array, values[i]);
    }
    sage_gc_unpin();
    return array;
}

static int sage_truthy(SageValue value) {
    if (value.type == SAGE_TAG_NIL) return 0;
    if (value.type == SAGE_TAG_BOOL) return value.as.boolean;
    if (value.type == SAGE_TAG_NUMBER) return value.as.number != 0.0;
    if (value.type == SAGE_TAG_STRING) return value.as.string[0] != '\0';
    return 1;
}

static SageValue sage_load_slot(const SageSlot* slot, const char* name) {
    if (!slot->defined) {
        fprintf(stderr, "Runtime Error: Undefined variable '%s'.\n", name);
        exit(1);
    }
    return slot->value;
}

static void sage_define_slot(SageSlot* slot, SageValue value) {
    slot->defined = 1;
    slot->value = value;
}

static SageValue sage_assign_slot(SageSlot* slot, const char* name, SageValue value) {
    if (!slot->defined) {
        fprintf(stderr, "Runtime Error: Undefined variable '%s'.\n", name);
        exit(1);
    }
    slot->value = value;
    return value;
}

static int sage_values_equal(SageValue left, SageValue right) {
    if (left.type != right.type) return 0;
    switch (left.type) {
        case SAGE_TAG_NIL: return 1;
        case SAGE_TAG_NUMBER: return left.as.number == right.as.number;
        case SAGE_TAG_BOOL: return left.as.boolean == right.as.boolean;
        case SAGE_TAG_STRING: return strcmp(left.as.string, right.as.string) == 0;
        case SAGE_TAG_ARRAY: {
            if (left.as.array == right.as.array) return 1;
            if (left.as.array->count != right.as.array->count) return 0;
            for (int i = 0; i < left.as.array->count; i++) {
                if (!sage_values_equal(left.as.array->elements[i], right.as.array->elements[i])) return 0;
            }
            return 1;
        }
        case SAGE_TAG_DICT: return left.as.dict == right.as.dict;
        case SAGE_TAG_TUPLE: {
            if (left.as.tuple == right.as.tuple) return 1;
            if (left.as.tuple->count != right.as.tuple->count) return 0;
            for (int i = 0; i < left.as.tuple->count; i++) {
                if (!sage_values_equal(left.as.tuple->elements[i], right.as.tuple->elements[i])) return 0;
            }
            return 1;
        }
    }
    return 0;
}

static void sage_print_value(SageValue value) {
    switch (value.type) {
        case SAGE_TAG_NUMBER: printf("%g", value.as.number); break;
        case SAGE_TAG_BOOL: fputs(value.as.boolean ? "true" : "false", stdout); break;
        case SAGE_TAG_STRING: fputs(value.as.string, stdout); break;
        case SAGE_TAG_ARRAY:
            fputc('[', stdout);
            for (int i = 0; i < value.as.array->count; i++) {
                if (i > 0) fputs(", ", stdout);
                sage_print_value(value.as.array->elements[i]);
            }
            fputc(']', stdout);
            break;
        case SAGE_TAG_DICT:
            fputc('{', stdout);
            for (int i = 0; i < value.as.dict->count; i++) {
                if (i > 0) fputs(", ", stdout);
                printf("\"%s\": ", value.as.dict->keys[i]);
                sage_print_value(value.as.dict->values[i]);
            }
            fputc('}', stdout);
            break;
        case SAGE_TAG_TUPLE:
            fputc('(', stdout);
            for (int i = 0; i < value.as.tuple->count; i++) {
                if (i > 0) fputs(", ", stdout);
                sage_print_value(value.as.tuple->elements[i]);
            }
            fputc(')', stdout);
            break;
        case SAGE_TAG_NIL: fputs("nil", stdout); break;
    }
}

static void sage_print_ln(SageValue value) {
    sage_print_value(value);
    fputc('\n', stdout);
}

static SageValue sage_str(SageValue value) {
    char buffer[64];
    switch (value.type) {
        case SAGE_TAG_STRING: return value;
        case SAGE_TAG_NUMBER:
            snprintf(buffer, sizeof(buffer), "%g", value.as.number);
            return sage_string(buffer);
        case SAGE_TAG_BOOL:
            return sage_string(value.as.boolean ? "true" : "false");
        case SAGE_TAG_NIL:
            return sage_string("nil");
        case SAGE_TAG_ARRAY:
            return sage_string("<array>");
        case SAGE_TAG_DICT:
            return sage_string("<dict>");
        case SAGE_TAG_TUPLE:
            return sage_string("<tuple>");
    }
    return sage_string("nil");
}

static SageValue sage_len(SageValue value) {
    if (value.type == SAGE_TAG_STRING) return sage_number((double)strlen(value.as.string));
    if (value.type == SAGE_TAG_ARRAY) return sage_number((double)value.as.array->count);
    if (value.type == SAGE_TAG_DICT) return sage_number((double)value.as.dict->count);
    if (value.type == SAGE_TAG_TUPLE) return sage_number((double)value.as.tuple->count);
    return sage_nil();
}

static SageValue sage_index(SageValue collection, SageValue index) {
    if (collection.type == SAGE_TAG_ARRAY && index.type == SAGE_TAG_NUMBER) {
        int idx = (int)index.as.number;
        if (idx < 0 || idx >= collection.as.array->count) return sage_nil();
        return collection.as.array->elements[idx];
    }
    if (collection.type == SAGE_TAG_DICT && index.type == SAGE_TAG_STRING) {
        return sage_dict_get(collection.as.dict, index.as.string);
    }
    if (collection.type == SAGE_TAG_TUPLE && index.type == SAGE_TAG_NUMBER) {
        int idx = (int)index.as.number;
        if (idx < 0 || idx >= collection.as.tuple->count) return sage_nil();
        return collection.as.tuple->elements[idx];
    }
    if (collection.type == SAGE_TAG_STRING && index.type == SAGE_TAG_NUMBER) {
        int idx = (int)index.as.number;
        int len = (int)strlen(collection.as.string);
        if (idx < 0 || idx >= len) return sage_nil();
        char buf[2] = {collection.as.string[idx], '\0'};
        return sage_string(buf);
    }
    return sage_nil();
}

static SageValue sage_slice(SageValue array, SageValue start, SageValue end) {
    if (array.type != SAGE_TAG_ARRAY) return sage_nil();
    sage_gc_pin();
    int start_index = 0;
    int end_index = array.as.array->count;
    if (start.type == SAGE_TAG_NUMBER) start_index = (int)start.as.number;
    else if (start.type != SAGE_TAG_NIL) { sage_gc_unpin(); return sage_nil(); }
    if (end.type == SAGE_TAG_NUMBER) end_index = (int)end.as.number;
    else if (end.type != SAGE_TAG_NIL) { sage_gc_unpin(); return sage_nil(); }
    if (start_index < 0) start_index = array.as.array->count + start_index;
    if (end_index < 0) end_index = array.as.array->count + end_index;
    if (start_index < 0) start_index = 0;
    if (end_index > array.as.array->count) end_index = array.as.array->count;
    if (start_index >= end_index) { SageValue empty = sage_array(); sage_gc_unpin(); return empty; }
    SageValue result = sage_array();
    for (int i = start_index; i < end_index; i++) {
        sage_array_push_raw(result.as.array, array.as.array->elements[i]);
    }
    sage_gc_unpin();
    return result;
}

static SageValue sage_push(SageValue array, SageValue value) {
    if (array.type != SAGE_TAG_ARRAY) return sage_nil();
    sage_array_push_raw(array.as.array, value);
    return sage_nil();
}

static SageValue sage_pop(SageValue array) {
    if (array.type != SAGE_TAG_ARRAY || array.as.array->count == 0) return sage_nil();
    return array.as.array->elements[--array.as.array->count];
}

static SageValue sage_range2(SageValue start, SageValue end) {
    if (start.type != SAGE_TAG_NUMBER || end.type != SAGE_TAG_NUMBER) return sage_nil();
    sage_gc_pin();
    SageValue result = sage_array();
    for (int i = (int)start.as.number; i < (int)end.as.number; i++) {
        sage_array_push_raw(result.as.array, sage_number((double)i));
    }
    sage_gc_unpin();
    return result;
}

static SageValue sage_range1(SageValue end) {
    return sage_range2(sage_number(0), end);
}

static SageValue sage_add(SageValue left, SageValue right) {
    if (left.type == SAGE_TAG_NUMBER && right.type == SAGE_TAG_NUMBER) {
        return sage_number(left.as.number + right.as.number);
    }
    if (left.type == SAGE_TAG_STRING && right.type == SAGE_TAG_STRING) {
        size_t len1 = strlen(left.as.string);
        size_t len2 = strlen(right.as.string);
        char* result = (char*)malloc(len1 + len2 + 1);
        if (result == NULL) sage_fail("Runtime Error: out of memory");
        memcpy(result, left.as.string, len1);
        memcpy(result + len1, right.as.string, len2 + 1);
        return sage_string_take(result);
    }
    sage_fail("Runtime Error: Operands must be numbers or strings.");
    return sage_nil();
}

static SageValue sage_sub(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number(left.as.number - right.as.number);
}
static SageValue sage_mul(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number(left.as.number * right.as.number);
}
static SageValue sage_div(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    if (right.as.number == 0) return sage_nil();
    return sage_number(left.as.number / right.as.number);
}
static SageValue sage_mod(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    if (right.as.number == 0) return sage_nil();
    return sage_number(fmod(left.as.number, right.as.number));
}
static SageValue sage_eq(SageValue left, SageValue right) { return sage_bool(sage_values_equal(left, right)); }
static SageValue sage_neq(SageValue left, SageValue right) { return sage_bool(!sage_values_equal(left, right)); }
static SageValue sage_gt(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_bool(left.as.number > right.as.number);
}
static SageValue sage_lt(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_bool(left.as.number < right.as.number);
}
static SageValue sage_gte(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_bool(left.as.number >= right.as.number);
}
static SageValue sage_lte(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_bool(left.as.number <= right.as.number);
}
static SageValue sage_not(SageValue value) { return sage_bool(!sage_truthy(value)); }
static SageValue sage_and(SageValue left, SageValue right) { return sage_bool(sage_truthy(left) && sage_truthy(right)); }
static SageValue sage_or(SageValue left, SageValue right) { return sage_bool(sage_truthy(left) || sage_truthy(right)); }
static SageValue sage_bit_not(SageValue value) {
    if (value.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Bitwise NOT operand must be a number.");
    return sage_number((double)(~(long long)value.as.number));
}
static SageValue sage_bit_and(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number((double)(((long long)left.as.number) & ((long long)right.as.number)));
}
static SageValue sage_bit_or(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number((double)(((long long)left.as.number) | ((long long)right.as.number)));
}
static SageValue sage_bit_xor(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number((double)(((long long)left.as.number) ^ ((long long)right.as.number)));
}
static SageValue sage_lshift(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number((double)(((long long)left.as.number) << ((long long)right.as.number)));
}
static SageValue sage_rshift(SageValue left, SageValue right) {
    if (left.type != SAGE_TAG_NUMBER || right.type != SAGE_TAG_NUMBER) sage_fail("Runtime Error: Operands must be numbers.");
    return sage_number((double)(((long long)left.as.number) >> ((long long)right.as.number)));
}

static SageValue sage_tonumber(SageValue value) {
    if (value.type == SAGE_TAG_NUMBER) return value;
    if (value.type == SAGE_TAG_STRING) {
        char* end;
        double result = strtod(value.as.string, &end);
        if (end != value.as.string && *end == '\0') return sage_number(result);
    }
    return sage_nil();
}

static SageValue sage_dict_keys_fn(SageValue dict_val) {
    if (dict_val.type != SAGE_TAG_DICT) return sage_array();
    sage_gc_pin();
    SageValue result = sage_array();
    for (int i = 0; i < dict_val.as.dict->count; i++) {
        sage_array_push_raw(result.as.array, sage_string(dict_val.as.dict->keys[i]));
    }
    sage_gc_unpin();
    return result;
}

static SageValue sage_dict_values_fn(SageValue dict_val) {
    if (dict_val.type != SAGE_TAG_DICT) return sage_array();
    sage_gc_pin();
    SageValue result = sage_array();
    for (int i = 0; i < dict_val.as.dict->count; i++) {
        sage_array_push_raw(result.as.array, dict_val.as.dict->values[i]);
    }
    sage_gc_unpin();
    return result;
}

static SageValue sage_dict_has_fn(SageValue dict_val, SageValue key) {
    if (dict_val.type != SAGE_TAG_DICT || key.type != SAGE_TAG_STRING) return sage_bool(0);
    for (int i = 0; i < dict_val.as.dict->count; i++) {
        if (strcmp(dict_val.as.dict->keys[i], key.as.string) == 0) return sage_bool(1);
    }
    return sage_bool(0);
}

static SageValue sage_dict_delete_fn(SageValue dict_val, SageValue key) {
    if (dict_val.type != SAGE_TAG_DICT || key.type != SAGE_TAG_STRING) return sage_nil();
    SageDict* dict = dict_val.as.dict;
    for (int i = 0; i < dict->count; i++) {
        if (strcmp(dict->keys[i], key.as.string) == 0) {
            free(dict->keys[i]);
            for (int j = i; j < dict->count - 1; j++) {
                dict->keys[j] = dict->keys[j + 1];
                dict->values[j] = dict->values[j + 1];
            }
            dict->count--;
            return sage_bool(1);
        }
    }
    return sage_bool(0);
}

static SageValue sage_chr(SageValue v) {
    if (v.type != SAGE_TAG_NUMBER) return sage_nil();
    char buf[2] = { (char)(int)v.as.number, 0 };
    return sage_string(buf);
}

static SageValue sage_ord(SageValue v) {
    if (v.type != SAGE_TAG_STRING || v.as.string == NULL || v.as.string[0] == 0) return sage_nil();
    return sage_number((double)(unsigned char)v.as.string[0]);
}

static SageValue sage_type(SageValue v) {
    switch (v.type) {
        case SAGE_TAG_NIL: return sage_string("nil");
        case SAGE_TAG_NUMBER: return sage_string("number");
        case SAGE_TAG_BOOL: return sage_string("bool");
        case SAGE_TAG_STRING: return sage_string("string");
        case SAGE_TAG_ARRAY: return sage_string("array");
        case SAGE_TAG_DICT: return sage_string("dict");
        default: return sage_string("unknown");
    }
}

static SageValue sage_startswith(SageValue s, SageValue prefix) {
    if (s.type != SAGE_TAG_STRING || prefix.type != SAGE_TAG_STRING) return sage_bool(0);
    return sage_bool(strncmp(s.as.string, prefix.as.string, strlen(prefix.as.string)) == 0);
}

static SageValue sage_endswith(SageValue s, SageValue suffix) {
    if (s.type != SAGE_TAG_STRING || suffix.type != SAGE_TAG_STRING) return sage_bool(0);
    size_t slen = strlen(s.as.string), suflen = strlen(suffix.as.string);
    if (suflen > slen) return sage_bool(0);
    return sage_bool(strcmp(s.as.string + slen - suflen, suffix.as.string) == 0);
}

static SageValue sage_contains(SageValue haystack, SageValue needle) {
    if (haystack.type != SAGE_TAG_STRING || needle.type != SAGE_TAG_STRING) return sage_bool(0);
    return sage_bool(strstr(haystack.as.string, needle.as.string) != NULL);
}

static void sage_index_set(SageValue c, SageValue k, SageValue v) {
    if (c.type == SAGE_TAG_ARRAY && k.type == SAGE_TAG_NUMBER) {
        int i = (int)k.as.number;
        if (i >= 0 && i < c.as.array->count) c.as.array->elements[i] = v;
        return;
    }
    if (c.type == SAGE_TAG_DICT && k.type == SAGE_TAG_STRING) {
        SageDict* d = c.as.dict;
        for (int i = 0; i < d->count; i++) {
            if (strcmp(d->keys[i], k.as.string) == 0) { d->values[i] = v; return; }
        }
        if (d->count >= d->capacity) {
            int nc = d->capacity == 0 ? 4 : d->capacity * 2;
            d->keys = realloc(d->keys, sizeof(char*) * nc);
            d->values = realloc(d->values, sizeof(SageValue) * nc);
            d->capacity = nc;
        }
        { size_t l = strlen(k.as.string); d->keys[d->count] = malloc(l+1); memcpy(d->keys[d->count], k.as.string, l+1); }
        d->values[d->count] = v;
        d->count++;
    }
}

static SageValue sage_gc_collect_fn(void) {
    sage_gc_collect();
    return sage_nil();
}

static SageValue sage_gc_enable_fn(void) {
    sage_gc.enabled = 1;
    return sage_nil();
}

static SageValue sage_gc_disable_fn(void) {
    sage_gc.enabled = 0;
    return sage_nil();
}

static SageValue sage_gc_stats_fn(void) {
    int next_gc = sage_gc.next_gc_objects - sage_gc.object_count;
    if (next_gc < 0) next_gc = 0;
    return sage_make_dict_from_entries(7,
        (const char*[]){"bytes_allocated", "current_bytes", "num_objects", "collections", "objects_freed", "next_gc", "next_gc_bytes"},
        (SageValue[]){
            sage_number((double)sage_gc.bytes_allocated),
            sage_number((double)sage_gc_live_bytes()),
            sage_number((double)sage_gc.object_count),
            sage_number((double)sage_gc.collections),
            sage_number(0),
            sage_number((double)next_gc),
            sage_number((double)sage_gc.next_gc_bytes)
        });
}

static SageValue sage_gc_collections_fn(void) {
    return sage_number((double)sage_gc.collections);
}

#include <ctype.h>
static SageValue sage_upper(SageValue value) {
    if (value.type != SAGE_TAG_STRING) return sage_nil();
    size_t len = strlen(value.as.string);
    char* result = (char*)malloc(len + 1);
    if (result == NULL) sage_fail("Runtime Error: out of memory");
    for (size_t i = 0; i < len; i++) result[i] = (char)toupper((unsigned char)value.as.string[i]);
    result[len] = '\0';
    return sage_string_take(result);
}
static SageValue sage_lower(SageValue value) {
    if (value.type != SAGE_TAG_STRING) return sage_nil();
    size_t len = strlen(value.as.string);
    char* result = (char*)malloc(len + 1);
    if (result == NULL) sage_fail("Runtime Error: out of memory");
    for (size_t i = 0; i < len; i++) result[i] = (char)tolower((unsigned char)value.as.string[i]);
    result[len] = '\0';
    return sage_string_take(result);
}
static SageValue sage_strip_fn(SageValue value) {
    if (value.type != SAGE_TAG_STRING) return sage_nil();
    const char* s = value.as.string;
    while (*s && isspace((unsigned char)*s)) s++;
    const char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - s);
    char* result = (char*)malloc(len + 1);
    if (result == NULL) sage_fail("Runtime Error: out of memory");
    memcpy(result, s, len);
    result[len] = '\0';
    return sage_string_take(result);
}

static SageValue sage_split_fn(SageValue str_val, SageValue delim_val) {
    if (str_val.type != SAGE_TAG_STRING || delim_val.type != SAGE_TAG_STRING) return sage_array();
    sage_gc_pin();
    const char* s = str_val.as.string;
    const char* delim = delim_val.as.string;
    size_t dlen = strlen(delim);
    SageValue result = sage_array();
    if (dlen == 0) {
        for (size_t i = 0; s[i]; i++) {
            char buf[2] = {s[i], '\0'};
            sage_array_push_raw(result.as.array, sage_string(buf));
        }
        sage_gc_unpin();
        return result;
    }
    const char* start = s;
    const char* found;
    while ((found = strstr(start, delim)) != NULL) {
        size_t len = (size_t)(found - start);
        char* part = (char*)malloc(len + 1);
        if (part == NULL) sage_fail("Runtime Error: out of memory");
        memcpy(part, start, len);
        part[len] = '\0';
        sage_array_push_raw(result.as.array, sage_string_take(part));
        start = found + dlen;
    }
    sage_array_push_raw(result.as.array, sage_string(start));
    sage_gc_unpin();
    return result;
}

static SageValue sage_join_fn(SageValue arr_val, SageValue delim_val) {
    if (arr_val.type != SAGE_TAG_ARRAY || delim_val.type != SAGE_TAG_STRING) return sage_nil();
    SageArray* arr = arr_val.as.array;
    const char* delim = delim_val.as.string;
    size_t dlen = strlen(delim);
    if (arr->count == 0) return sage_string("");
    size_t total = 0;
    for (int i = 0; i < arr->count; i++) {
        if (arr->elements[i].type == SAGE_TAG_STRING) total += strlen(arr->elements[i].as.string);
        if (i > 0) total += dlen;
    }
    char* result = (char*)malloc(total + 1);
    if (result == NULL) sage_fail("Runtime Error: out of memory");
    char* p = result;
    for (int i = 0; i < arr->count; i++) {
        if (i > 0) { memcpy(p, delim, dlen); p += dlen; }
        if (arr->elements[i].type == SAGE_TAG_STRING) {
            size_t len = strlen(arr->elements[i].as.string);
            memcpy(p, arr->elements[i].as.string, len);
            p += len;
        }
    }
    *p = '\0';
    return sage_string_take(result);
}

static SageValue sage_replace_fn(SageValue str_val, SageValue old_val, SageValue new_val) {
    if (str_val.type != SAGE_TAG_STRING || old_val.type != SAGE_TAG_STRING || new_val.type != SAGE_TAG_STRING)
        return sage_nil();
    const char* s = str_val.as.string;
    const char* old_s = old_val.as.string;
    const char* new_s = new_val.as.string;
    size_t old_len = strlen(old_s);
    size_t new_len = strlen(new_s);
    if (old_len == 0) return sage_string(s);
    size_t count = 0;
    const char* tmp = s;
    while ((tmp = strstr(tmp, old_s)) != NULL) { count++; tmp += old_len; }
    size_t result_len = strlen(s) + count * (new_len - old_len);
    char* result = (char*)malloc(result_len + 1);
    if (result == NULL) sage_fail("Runtime Error: out of memory");
    char* p = result;
    while (*s) {
        if (strncmp(s, old_s, old_len) == 0) {
            memcpy(p, new_s, new_len);
            p += new_len;
            s += old_len;
        } else {
            *p++ = *s++;
        }
    }
    *p = '\0';
    return sage_string_take(result);
}

#include <stdint.h>

typedef struct {
    void* ptr;
    size_t size;
    int owned;
} SagePointer;

static SageValue sage_mem_alloc(SageValue size_val) {
    if (size_val.type != SAGE_TAG_NUMBER) { fputs("mem_alloc(): expects number\n", stderr); return sage_nil(); }
    size_t size = (size_t)size_val.as.number;
    if (size == 0 || size > 1024*1024*64) { fputs("mem_alloc(): invalid size\n", stderr); return sage_nil(); }
    SagePointer* sp = (SagePointer*)malloc(sizeof(SagePointer));
    if (sp == NULL) sage_fail("Runtime Error: out of memory");
    sp->ptr = calloc(1, size);
    if (sp->ptr == NULL) { free(sp); sage_fail("Runtime Error: out of memory"); }
    sp->size = size;
    sp->owned = 1;
    SageValue v; v.type = SAGE_TAG_NUMBER; v.as.number = (double)(uintptr_t)sp;
    return v;
}

static SagePointer* sage_as_pointer(SageValue v) {
    if (v.type != SAGE_TAG_NUMBER) return NULL;
    return (SagePointer*)(uintptr_t)v.as.number;
}

static SageValue sage_mem_free(SageValue ptr_val) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL) { fputs("mem_free(): expects pointer\n", stderr); return sage_nil(); }
    if (sp->ptr && sp->owned) { free(sp->ptr); sp->ptr = NULL; sp->size = 0; }
    free(sp);
    return sage_nil();
}

static SageValue sage_mem_read(SageValue ptr_val, SageValue off_val, SageValue type_val) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL || sp->ptr == NULL || off_val.type != SAGE_TAG_NUMBER || type_val.type != SAGE_TAG_STRING)
        return sage_nil();
    size_t offset = (size_t)off_val.as.number;
    const char* type = type_val.as.string;
    unsigned char* base = (unsigned char*)sp->ptr + offset;
    if (strcmp(type, "byte") == 0) { return sage_number((double)*base); }
    if (strcmp(type, "int") == 0) { int v; memcpy(&v, base, sizeof(int)); return sage_number((double)v); }
    if (strcmp(type, "double") == 0) { double v; memcpy(&v, base, sizeof(double)); return sage_number(v); }
    if (strcmp(type, "string") == 0) { return sage_string((const char*)base); }
    return sage_nil();
}

static SageValue sage_mem_write(SageValue ptr_val, SageValue off_val, SageValue type_val, SageValue val) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL || sp->ptr == NULL || off_val.type != SAGE_TAG_NUMBER || type_val.type != SAGE_TAG_STRING)
        return sage_nil();
    size_t offset = (size_t)off_val.as.number;
    const char* type = type_val.as.string;
    unsigned char* base = (unsigned char*)sp->ptr + offset;
    if (strcmp(type, "byte") == 0 && val.type == SAGE_TAG_NUMBER) { *base = (unsigned char)val.as.number; }
    else if (strcmp(type, "int") == 0 && val.type == SAGE_TAG_NUMBER) { int v = (int)val.as.number; memcpy(base, &v, sizeof(int)); }
    else if (strcmp(type, "double") == 0 && val.type == SAGE_TAG_NUMBER) { double v = val.as.number; memcpy(base, &v, sizeof(double)); }
    return sage_nil();
}

static SageValue sage_mem_size(SageValue ptr_val) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL) return sage_nil();
    return sage_number((double)sp->size);
}

static int sage_struct_type_info(const char* type, size_t* out_size, size_t* out_align) {
    if (strcmp(type,"char")==0||strcmp(type,"byte")==0) { *out_size=1; *out_align=1; return 0; }
    if (strcmp(type,"short")==0) { *out_size=sizeof(short); *out_align=sizeof(short); return 0; }
    if (strcmp(type,"int")==0) { *out_size=sizeof(int); *out_align=sizeof(int); return 0; }
    if (strcmp(type,"long")==0) { *out_size=sizeof(long); *out_align=sizeof(long); return 0; }
    if (strcmp(type,"float")==0) { *out_size=sizeof(float); *out_align=sizeof(float); return 0; }
    if (strcmp(type,"double")==0) { *out_size=sizeof(double); *out_align=sizeof(double); return 0; }
    if (strcmp(type,"ptr")==0) { *out_size=sizeof(void*); *out_align=sizeof(void*); return 0; }
    return -1;
}

static SageValue sage_struct_def(SageValue fields) {
    if (fields.type != SAGE_TAG_ARRAY) return sage_nil();
    sage_gc_pin();
    SageValue def = sage_make_dict();
    size_t offset = 0, max_align = 1;
    for (int i = 0; i < fields.as.array->count; i++) {
        SageValue pair = fields.as.array->elements[i];
        if (pair.type != SAGE_TAG_ARRAY || pair.as.array->count < 2) continue;
        if (pair.as.array->elements[0].type != SAGE_TAG_STRING ||
            pair.as.array->elements[1].type != SAGE_TAG_STRING) continue;
        const char* name = pair.as.array->elements[0].as.string;
        const char* type = pair.as.array->elements[1].as.string;
        size_t fsize, falign;
        if (sage_struct_type_info(type, &fsize, &falign) != 0) continue;
        if (falign > max_align) max_align = falign;
        size_t rem = offset % falign;
        if (rem != 0) offset += falign - rem;
        /* store field: "name" -> [offset, type] */
        SageValue field_info = sage_make_array(2, (SageValue[]){
            sage_number((double)offset), sage_string(type)
        });
        sage_dict_set(def.as.dict, name, field_info);
        offset += fsize;
    }
    size_t rem = offset % max_align;
    if (rem != 0) offset += max_align - rem;
    sage_dict_set(def.as.dict, "__size__", sage_number((double)offset));
    sage_dict_set(def.as.dict, "__align__", sage_number((double)max_align));
    sage_gc_unpin();
    return def;
}

static SageValue sage_struct_new(SageValue def) {
    if (def.type != SAGE_TAG_DICT) return sage_nil();
    SageValue size_val = sage_dict_get(def.as.dict, "__size__");
    if (size_val.type != SAGE_TAG_NUMBER) return sage_nil();
    size_t size = (size_t)size_val.as.number;
    SagePointer* sp = (SagePointer*)malloc(sizeof(SagePointer));
    if (sp == NULL) sage_fail("Runtime Error: out of memory");
    sp->ptr = calloc(1, size);
    if (sp->ptr == NULL) { free(sp); sage_fail("Runtime Error: out of memory"); }
    sp->size = size;
    sp->owned = 1;
    SageValue v; v.type = SAGE_TAG_NUMBER; v.as.number = (double)(uintptr_t)sp;
    return v;
}

static SageValue sage_struct_get(SageValue ptr_val, SageValue def, SageValue field_name) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL || sp->ptr == NULL || def.type != SAGE_TAG_DICT || field_name.type != SAGE_TAG_STRING)
        return sage_nil();
    SageValue info = sage_dict_get(def.as.dict, field_name.as.string);
    if (info.type != SAGE_TAG_ARRAY || info.as.array->count < 2) return sage_nil();
    size_t offset = (size_t)info.as.array->elements[0].as.number;
    const char* type = info.as.array->elements[1].as.string;
    unsigned char* base = (unsigned char*)sp->ptr + offset;
    if (strcmp(type,"char")==0||strcmp(type,"byte")==0) return sage_number((double)*base);
    if (strcmp(type,"short")==0) { short v; memcpy(&v,base,sizeof(short)); return sage_number((double)v); }
    if (strcmp(type,"int")==0) { int v; memcpy(&v,base,sizeof(int)); return sage_number((double)v); }
    if (strcmp(type,"long")==0) { long v; memcpy(&v,base,sizeof(long)); return sage_number((double)v); }
    if (strcmp(type,"float")==0) { float v; memcpy(&v,base,sizeof(float)); return sage_number((double)v); }
    if (strcmp(type,"double")==0) { double v; memcpy(&v,base,sizeof(double)); return sage_number(v); }
    return sage_nil();
}

static SageValue sage_struct_set(SageValue ptr_val, SageValue def, SageValue field_name, SageValue val) {
    SagePointer* sp = sage_as_pointer(ptr_val);
    if (sp == NULL || sp->ptr == NULL || def.type != SAGE_TAG_DICT || field_name.type != SAGE_TAG_STRING)
        return sage_nil();
    SageValue info = sage_dict_get(def.as.dict, field_name.as.string);
    if (info.type != SAGE_TAG_ARRAY || info.as.array->count < 2) return sage_nil();
    size_t offset = (size_t)info.as.array->elements[0].as.number;
    const char* type = info.as.array->elements[1].as.string;
    unsigned char* base = (unsigned char*)sp->ptr + offset;
    if (val.type != SAGE_TAG_NUMBER) return sage_nil();
    if (strcmp(type,"char")==0||strcmp(type,"byte")==0) { *base = (unsigned char)val.as.number; }
    else if (strcmp(type,"short")==0) { short v=(short)val.as.number; memcpy(base,&v,sizeof(short)); }
    else if (strcmp(type,"int")==0) { int v=(int)val.as.number; memcpy(base,&v,sizeof(int)); }
    else if (strcmp(type,"long")==0) { long v=(long)val.as.number; memcpy(base,&v,sizeof(long)); }
    else if (strcmp(type,"float")==0) { float v=(float)val.as.number; memcpy(base,&v,sizeof(float)); }
    else if (strcmp(type,"double")==0) { double v=val.as.number; memcpy(base,&v,sizeof(double)); }
    return sage_nil();
}

static SageValue sage_struct_size(SageValue def) {
    if (def.type != SAGE_TAG_DICT) return sage_nil();
    return sage_dict_get(def.as.dict, "__size__");
}

typedef SageValue (*SageMethodFn)(SageValue, int, SageValue*);
typedef struct { const char* class_name; const char* method_name; SageMethodFn fn; } SageMethodEntry;
typedef struct { const char* name; const char* parent; } SageClassEntry;
#define SAGE_MAX_METHODS 256
#define SAGE_MAX_CLASSES 64
static SageMethodEntry sage_method_table[SAGE_MAX_METHODS];
static int sage_method_count = 0;
static SageClassEntry sage_class_registry[SAGE_MAX_CLASSES];
static int sage_class_count = 0;

static void sage_register_class(const char* name, const char* parent) {
    if (sage_class_count >= SAGE_MAX_CLASSES) sage_fail("too many classes");
    sage_class_registry[sage_class_count].name = name;
    sage_class_registry[sage_class_count].parent = parent;
    sage_class_count++;
}

static void sage_register_method(const char* cls, const char* name, SageMethodFn fn) {
    if (sage_method_count >= SAGE_MAX_METHODS) sage_fail("too many methods");
    sage_method_table[sage_method_count].class_name = cls;
    sage_method_table[sage_method_count].method_name = name;
    sage_method_table[sage_method_count].fn = fn;
    sage_method_count++;
}

static SageValue sage_call_method(SageValue obj, const char* method, int argc, SageValue* argv) {
    if (obj.type != SAGE_TAG_DICT) {
        fprintf(stderr, "Runtime Error: method call on non-instance.\n");
        exit(1);
    }
    SageValue class_val = sage_dict_get(obj.as.dict, "__class__");
    if (class_val.type != SAGE_TAG_STRING) {
        fprintf(stderr, "Runtime Error: no __class__ on instance.\n");
        exit(1);
    }
    const char* current = class_val.as.string;
    while (current != NULL) {
        for (int i = 0; i < sage_method_count; i++) {
            if (strcmp(sage_method_table[i].class_name, current) == 0 &&
                strcmp(sage_method_table[i].method_name, method) == 0) {
                return sage_method_table[i].fn(obj, argc, argv);
            }
        }
        const char* parent = NULL;
        for (int j = 0; j < sage_class_count; j++) {
            if (strcmp(sage_class_registry[j].name, current) == 0) {
                parent = sage_class_registry[j].parent;
                break;
            }
        }
        current = parent;
    }
    fprintf(stderr, "Runtime Error: Undefined method '%s'.\n", method);
    exit(1);
    return sage_nil();
}

static SageValue sage_construct(const char* class_name, const char* parent_name, int argc, SageValue* argv) {
    sage_gc_pin();
    SageValue inst = sage_make_dict();
    sage_dict_set(inst.as.dict, "__class__", sage_string(class_name));
    if (parent_name != NULL) sage_dict_set(inst.as.dict, "__parent__", sage_string(parent_name));
    sage_gc_unpin();
    const char* current = class_name;
    while (current != NULL) {
        for (int i = 0; i < sage_method_count; i++) {
            if (strcmp(sage_method_table[i].class_name, current) == 0 &&
                strcmp(sage_method_table[i].method_name, "init") == 0) {
                sage_method_table[i].fn(inst, argc, argv);
                return inst;
            }
        }
        const char* parent = NULL;
        for (int j = 0; j < sage_class_count; j++) {
            if (strcmp(sage_class_registry[j].name, current) == 0) {
                parent = sage_class_registry[j].parent;
                break;
            }
        }
        current = parent;
    }
    return inst;
}

static SageValue sage_arch_fn(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return sage_string("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return sage_string("aarch64");
#elif defined(__riscv) && __riscv_xlen == 64
    return sage_string("rv64");
#else
    return sage_string("unknown");
#endif
}

#include <time.h>
static SageValue sage_clock_fn(void) {
    return sage_number((double)clock() / CLOCKS_PER_SEC);
}
static SageValue sage_input_fn(SageValue prompt) {
    if (prompt.type == SAGE_TAG_STRING) fputs(prompt.as.string, stdout);
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) == NULL) return sage_nil();
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return sage_string(buf);
}

static SageValue sage_fn_table_14(SageValue arg0, SageValue arg1);
static SageValue sage_fn_repeat_char_13(SageValue arg0, SageValue arg1);
static SageValue sage_fn_join_12(SageValue arg0, SageValue arg1);
static SageValue sage_fn_template_11(SageValue arg0, SageValue arg1);
static SageValue sage_fn_to_oct_10(SageValue arg0);
static SageValue sage_fn_to_bin_9(SageValue arg0, SageValue arg1);
static SageValue sage_fn_to_hex_8(SageValue arg0, SageValue arg1);
static SageValue sage_fn_format_duration_7(SageValue arg0);
static SageValue sage_fn_format_bytes_6(SageValue arg0);
static SageValue sage_fn_format_pct_5(SageValue arg0, SageValue arg1);
static SageValue sage_fn_format_float_4(SageValue arg0, SageValue arg1);
static SageValue sage_fn_format_int_3(SageValue arg0);
static SageValue sage_fn_pad_right_2(SageValue arg0, SageValue arg1, SageValue arg2);
static SageValue sage_fn_pad_left_1(SageValue arg0, SageValue arg1, SageValue arg2);

static SageValue sage_fn_pad_left_1(SageValue arg0, SageValue arg1, SageValue arg2) {
    SageSlot sage_local_result_18 = sage_slot_undefined();
    SageSlot sage_param_ch_17 = sage_slot_undefined();
    SageSlot sage_param_width_16 = sage_slot_undefined();
    SageSlot sage_param_s_15 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_result_18, &sage_param_ch_17, &sage_param_width_16, &sage_param_s_15};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_s_15, arg0);
    sage_define_slot(&sage_param_width_16, arg1);
    sage_define_slot(&sage_param_ch_17, arg2);
    sage_define_slot(&sage_local_result_18, sage_load_slot(&sage_param_s_15, "s"));
    while (sage_truthy(sage_lt(sage_len(sage_load_slot(&sage_local_result_18, "result")), sage_load_slot(&sage_param_width_16, "width")))) {
        (void)sage_assign_slot(&sage_local_result_18, "result", sage_add(sage_load_slot(&sage_param_ch_17, "ch"), sage_load_slot(&sage_local_result_18, "result")));
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_18, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_pad_right_2(SageValue arg0, SageValue arg1, SageValue arg2) {
    SageSlot sage_local_result_22 = sage_slot_undefined();
    SageSlot sage_param_ch_21 = sage_slot_undefined();
    SageSlot sage_param_width_20 = sage_slot_undefined();
    SageSlot sage_param_s_19 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_result_22, &sage_param_ch_21, &sage_param_width_20, &sage_param_s_19};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_s_19, arg0);
    sage_define_slot(&sage_param_width_20, arg1);
    sage_define_slot(&sage_param_ch_21, arg2);
    sage_define_slot(&sage_local_result_22, sage_load_slot(&sage_param_s_19, "s"));
    while (sage_truthy(sage_lt(sage_len(sage_load_slot(&sage_local_result_22, "result")), sage_load_slot(&sage_param_width_20, "width")))) {
        (void)sage_assign_slot(&sage_local_result_22, "result", sage_add(sage_load_slot(&sage_local_result_22, "result"), sage_load_slot(&sage_param_ch_21, "ch")));
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_22, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_format_int_3(SageValue arg0) {
    SageSlot sage_local_count_29 = sage_slot_undefined();
    SageSlot sage_local_result_28 = sage_slot_undefined();
    SageSlot sage_local_i_27 = sage_slot_undefined();
    SageSlot sage_local_tmp_26 = sage_slot_undefined();
    SageSlot sage_local_neg_25 = sage_slot_undefined();
    SageSlot sage_local_s_24 = sage_slot_undefined();
    SageSlot sage_param_n_23 = sage_slot_undefined();
    SageSlot* sage_gc_roots[7] = {&sage_local_count_29, &sage_local_result_28, &sage_local_i_27, &sage_local_tmp_26, &sage_local_neg_25, &sage_local_s_24, &sage_param_n_23};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 7);
    sage_define_slot(&sage_param_n_23, arg0);
    sage_define_slot(&sage_local_s_24, sage_str(sage_load_slot(&sage_param_n_23, "n")));
    sage_define_slot(&sage_local_neg_25, sage_bool(0));
    if (sage_truthy(sage_and(sage_gt(sage_len(sage_load_slot(&sage_local_s_24, "s")), sage_number(0)), sage_eq(sage_index(sage_load_slot(&sage_local_s_24, "s"), sage_number(0)), sage_string("-"))))) {
        (void)sage_assign_slot(&sage_local_neg_25, "neg", sage_bool(1));
        sage_define_slot(&sage_local_tmp_26, sage_string(""));
        {
            SageValue sage_iter_i_30 = sage_range1(sage_sub(sage_len(sage_load_slot(&sage_local_s_24, "s")), sage_number(1)));
            if (sage_iter_i_30.type == SAGE_TAG_ARRAY) {
                for (int sage_idx_i_31 = 0; sage_idx_i_31 < sage_iter_i_30.as.array->count; sage_idx_i_31++) {
                    sage_define_slot(&sage_local_i_27, sage_iter_i_30.as.array->elements[sage_idx_i_31]);
                        (void)sage_assign_slot(&sage_local_tmp_26, "tmp", sage_add(sage_load_slot(&sage_local_tmp_26, "tmp"), sage_index(sage_load_slot(&sage_local_s_24, "s"), sage_add(sage_load_slot(&sage_local_i_27, "i"), sage_number(1)))));
                }
            } else if (sage_iter_i_30.type == SAGE_TAG_STRING) {
                int _len = (int)strlen(sage_iter_i_30.as.string);
                for (int sage_idx_i_31 = 0; sage_idx_i_31 < _len; sage_idx_i_31++) {
                    char _ch[2] = {sage_iter_i_30.as.string[sage_idx_i_31], '\0'};
                    sage_define_slot(&sage_local_i_27, sage_string(_ch));
                        (void)sage_assign_slot(&sage_local_tmp_26, "tmp", sage_add(sage_load_slot(&sage_local_tmp_26, "tmp"), sage_index(sage_load_slot(&sage_local_s_24, "s"), sage_add(sage_load_slot(&sage_local_i_27, "i"), sage_number(1)))));
                }
            }
        }
        (void)sage_assign_slot(&sage_local_s_24, "s", sage_load_slot(&sage_local_tmp_26, "tmp"));
    }
    if (sage_truthy(sage_lte(sage_len(sage_load_slot(&sage_local_s_24, "s")), sage_number(3)))) {
        if (sage_truthy(sage_load_slot(&sage_local_neg_25, "neg"))) {
            return sage_gc_return(&sage_gc_frame, sage_add(sage_string("-"), sage_load_slot(&sage_local_s_24, "s")));
        }
        return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_s_24, "s"));
    }
    sage_define_slot(&sage_local_result_28, sage_string(""));
    sage_define_slot(&sage_local_count_29, sage_number(0));
    sage_define_slot(&sage_local_i_27, sage_sub(sage_len(sage_load_slot(&sage_local_s_24, "s")), sage_number(1)));
    while (sage_truthy(sage_gte(sage_load_slot(&sage_local_i_27, "i"), sage_number(0)))) {
        if (sage_truthy(sage_and(sage_gt(sage_load_slot(&sage_local_count_29, "count"), sage_number(0)), sage_eq(sage_sub(sage_load_slot(&sage_local_count_29, "count"), sage_mul(sage_bit_or(sage_div(sage_load_slot(&sage_local_count_29, "count"), sage_number(3)), sage_number(0)), sage_number(3))), sage_number(0))))) {
            (void)sage_assign_slot(&sage_local_result_28, "result", sage_add(sage_string(","), sage_load_slot(&sage_local_result_28, "result")));
        }
        (void)sage_assign_slot(&sage_local_result_28, "result", sage_add(sage_index(sage_load_slot(&sage_local_s_24, "s"), sage_load_slot(&sage_local_i_27, "i")), sage_load_slot(&sage_local_result_28, "result")));
        (void)sage_assign_slot(&sage_local_count_29, "count", sage_add(sage_load_slot(&sage_local_count_29, "count"), sage_number(1)));
        (void)sage_assign_slot(&sage_local_i_27, "i", sage_sub(sage_load_slot(&sage_local_i_27, "i"), sage_number(1)));
    }
    if (sage_truthy(sage_load_slot(&sage_local_neg_25, "neg"))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_string("-"), sage_load_slot(&sage_local_result_28, "result")));
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_28, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_format_float_4(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_digit_38 = sage_slot_undefined();
    SageSlot sage_local_i_37 = sage_slot_undefined();
    SageSlot sage_local_result_36 = sage_slot_undefined();
    SageSlot sage_local_frac_35 = sage_slot_undefined();
    SageSlot sage_local_int_part_34 = sage_slot_undefined();
    SageSlot sage_param_decimals_33 = sage_slot_undefined();
    SageSlot sage_param_n_32 = sage_slot_undefined();
    SageSlot* sage_gc_roots[7] = {&sage_local_digit_38, &sage_local_i_37, &sage_local_result_36, &sage_local_frac_35, &sage_local_int_part_34, &sage_param_decimals_33, &sage_param_n_32};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 7);
    sage_define_slot(&sage_param_n_32, arg0);
    sage_define_slot(&sage_param_decimals_33, arg1);
    sage_define_slot(&sage_local_int_part_34, sage_bit_or(sage_load_slot(&sage_param_n_32, "n"), sage_number(0)));
    sage_define_slot(&sage_local_frac_35, sage_sub(sage_load_slot(&sage_param_n_32, "n"), sage_load_slot(&sage_local_int_part_34, "int_part")));
    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_frac_35, "frac", sage_sub(sage_number(0), sage_load_slot(&sage_local_frac_35, "frac")));
    }
    sage_define_slot(&sage_local_result_36, sage_add(sage_str(sage_load_slot(&sage_local_int_part_34, "int_part")), sage_string(".")));
    {
        SageValue sage_iter_i_39 = sage_range1(sage_load_slot(&sage_param_decimals_33, "decimals"));
        if (sage_iter_i_39.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_40 = 0; sage_idx_i_40 < sage_iter_i_39.as.array->count; sage_idx_i_40++) {
                sage_define_slot(&sage_local_i_37, sage_iter_i_39.as.array->elements[sage_idx_i_40]);
                    (void)sage_assign_slot(&sage_local_frac_35, "frac", sage_mul(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(10)));
                    sage_define_slot(&sage_local_digit_38, sage_sub(sage_bit_or(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(0)), sage_mul(sage_bit_or(sage_div(sage_bit_or(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(0)), sage_number(10)), sage_number(0)), sage_number(10))));
                    (void)sage_assign_slot(&sage_local_result_36, "result", sage_add(sage_load_slot(&sage_local_result_36, "result"), sage_str(sage_load_slot(&sage_local_digit_38, "digit"))));
            }
        } else if (sage_iter_i_39.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_39.as.string);
            for (int sage_idx_i_40 = 0; sage_idx_i_40 < _len; sage_idx_i_40++) {
                char _ch[2] = {sage_iter_i_39.as.string[sage_idx_i_40], '\0'};
                sage_define_slot(&sage_local_i_37, sage_string(_ch));
                    (void)sage_assign_slot(&sage_local_frac_35, "frac", sage_mul(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(10)));
                    sage_define_slot(&sage_local_digit_38, sage_sub(sage_bit_or(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(0)), sage_mul(sage_bit_or(sage_div(sage_bit_or(sage_load_slot(&sage_local_frac_35, "frac"), sage_number(0)), sage_number(10)), sage_number(0)), sage_number(10))));
                    (void)sage_assign_slot(&sage_local_result_36, "result", sage_add(sage_load_slot(&sage_local_result_36, "result"), sage_str(sage_load_slot(&sage_local_digit_38, "digit"))));
            }
        }
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_36, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_format_pct_5(SageValue arg0, SageValue arg1) {
    SageSlot sage_param_decimals_42 = sage_slot_undefined();
    SageSlot sage_param_n_41 = sage_slot_undefined();
    SageSlot* sage_gc_roots[2] = {&sage_param_decimals_42, &sage_param_n_41};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 2);
    sage_define_slot(&sage_param_n_41, arg0);
    sage_define_slot(&sage_param_decimals_42, arg1);
    return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_mul(sage_load_slot(&sage_param_n_41, "n"), sage_number(100)), sage_load_slot(&sage_param_decimals_42, "decimals")), sage_string("%")));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_format_bytes_6(SageValue arg0) {
    SageSlot sage_param_n_43 = sage_slot_undefined();
    SageSlot* sage_gc_roots[1] = {&sage_param_n_43};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 1);
    sage_define_slot(&sage_param_n_43, arg0);
    if (sage_truthy(sage_gte(sage_load_slot(&sage_param_n_43, "n"), sage_number(1073741824)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_div(sage_load_slot(&sage_param_n_43, "n"), sage_number(1073741824)), sage_number(1)), sage_string(" GB")));
    }
    if (sage_truthy(sage_gte(sage_load_slot(&sage_param_n_43, "n"), sage_number(1048576)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_div(sage_load_slot(&sage_param_n_43, "n"), sage_number(1048576)), sage_number(1)), sage_string(" MB")));
    }
    if (sage_truthy(sage_gte(sage_load_slot(&sage_param_n_43, "n"), sage_number(1024)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_div(sage_load_slot(&sage_param_n_43, "n"), sage_number(1024)), sage_number(1)), sage_string(" KB")));
    }
    return sage_gc_return(&sage_gc_frame, sage_add(sage_str(sage_load_slot(&sage_param_n_43, "n")), sage_string(" B")));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_format_duration_7(SageValue arg0) {
    SageSlot sage_local_hrs_47 = sage_slot_undefined();
    SageSlot sage_local_s_46 = sage_slot_undefined();
    SageSlot sage_local_mins_45 = sage_slot_undefined();
    SageSlot sage_param_secs_44 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_hrs_47, &sage_local_s_46, &sage_local_mins_45, &sage_param_secs_44};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_secs_44, arg0);
    if (sage_truthy(sage_lt(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(0.001)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_mul(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(1000000)), sage_number(0)), sage_string("us")));
    }
    if (sage_truthy(sage_lt(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(1)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_mul(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(1000)), sage_number(1)), sage_string("ms")));
    }
    if (sage_truthy(sage_lt(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(60)))) {
        return sage_gc_return(&sage_gc_frame, sage_add(sage_fn_format_float_4(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(2)), sage_string("s")));
    }
    if (sage_truthy(sage_lt(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(3600)))) {
        sage_define_slot(&sage_local_mins_45, sage_bit_or(sage_div(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(60)), sage_number(0)));
        sage_define_slot(&sage_local_s_46, sage_sub(sage_load_slot(&sage_param_secs_44, "secs"), sage_mul(sage_load_slot(&sage_local_mins_45, "mins"), sage_number(60))));
        return sage_gc_return(&sage_gc_frame, sage_add(sage_add(sage_add(sage_str(sage_load_slot(&sage_local_mins_45, "mins")), sage_string("m ")), sage_str(sage_bit_or(sage_load_slot(&sage_local_s_46, "s"), sage_number(0)))), sage_string("s")));
    }
    sage_define_slot(&sage_local_hrs_47, sage_bit_or(sage_div(sage_load_slot(&sage_param_secs_44, "secs"), sage_number(3600)), sage_number(0)));
    sage_define_slot(&sage_local_mins_45, sage_bit_or(sage_div(sage_sub(sage_load_slot(&sage_param_secs_44, "secs"), sage_mul(sage_load_slot(&sage_local_hrs_47, "hrs"), sage_number(3600))), sage_number(60)), sage_number(0)));
    return sage_gc_return(&sage_gc_frame, sage_add(sage_add(sage_add(sage_str(sage_load_slot(&sage_local_hrs_47, "hrs")), sage_string("h ")), sage_str(sage_load_slot(&sage_local_mins_45, "mins"))), sage_string("m")));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_to_hex_8(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_val_52 = sage_slot_undefined();
    SageSlot sage_local_result_51 = sage_slot_undefined();
    SageSlot sage_local_digits_50 = sage_slot_undefined();
    SageSlot sage_param_width_49 = sage_slot_undefined();
    SageSlot sage_param_n_48 = sage_slot_undefined();
    SageSlot* sage_gc_roots[5] = {&sage_local_val_52, &sage_local_result_51, &sage_local_digits_50, &sage_param_width_49, &sage_param_n_48};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 5);
    sage_define_slot(&sage_param_n_48, arg0);
    sage_define_slot(&sage_param_width_49, arg1);
    sage_define_slot(&sage_local_digits_50, sage_string("0123456789abcdef"));
    sage_define_slot(&sage_local_result_51, sage_string(""));
    sage_define_slot(&sage_local_val_52, sage_load_slot(&sage_param_n_48, "n"));
    if (sage_truthy(sage_eq(sage_load_slot(&sage_local_val_52, "val"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_result_51, "result", sage_string("0"));
    }
    while (sage_truthy(sage_gt(sage_load_slot(&sage_local_val_52, "val"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_result_51, "result", sage_add(sage_index(sage_load_slot(&sage_local_digits_50, "digits"), sage_bit_and(sage_load_slot(&sage_local_val_52, "val"), sage_number(15))), sage_load_slot(&sage_local_result_51, "result")));
        (void)sage_assign_slot(&sage_local_val_52, "val", sage_rshift(sage_load_slot(&sage_local_val_52, "val"), sage_number(4)));
    }
    return sage_gc_return(&sage_gc_frame, sage_add(sage_string("0x"), sage_fn_pad_left_1(sage_load_slot(&sage_local_result_51, "result"), sage_load_slot(&sage_param_width_49, "width"), sage_string("0"))));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_to_bin_9(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_val_56 = sage_slot_undefined();
    SageSlot sage_local_result_55 = sage_slot_undefined();
    SageSlot sage_param_width_54 = sage_slot_undefined();
    SageSlot sage_param_n_53 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_val_56, &sage_local_result_55, &sage_param_width_54, &sage_param_n_53};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_n_53, arg0);
    sage_define_slot(&sage_param_width_54, arg1);
    sage_define_slot(&sage_local_result_55, sage_string(""));
    sage_define_slot(&sage_local_val_56, sage_load_slot(&sage_param_n_53, "n"));
    if (sage_truthy(sage_eq(sage_load_slot(&sage_local_val_56, "val"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_result_55, "result", sage_string("0"));
    }
    while (sage_truthy(sage_gt(sage_load_slot(&sage_local_val_56, "val"), sage_number(0)))) {
        if (sage_truthy(sage_neq(sage_bit_and(sage_load_slot(&sage_local_val_56, "val"), sage_number(1)), sage_number(0)))) {
            (void)sage_assign_slot(&sage_local_result_55, "result", sage_add(sage_string("1"), sage_load_slot(&sage_local_result_55, "result")));
        }
        else {
            (void)sage_assign_slot(&sage_local_result_55, "result", sage_add(sage_string("0"), sage_load_slot(&sage_local_result_55, "result")));
        }
        (void)sage_assign_slot(&sage_local_val_56, "val", sage_rshift(sage_load_slot(&sage_local_val_56, "val"), sage_number(1)));
    }
    return sage_gc_return(&sage_gc_frame, sage_add(sage_string("0b"), sage_fn_pad_left_1(sage_load_slot(&sage_local_result_55, "result"), sage_load_slot(&sage_param_width_54, "width"), sage_string("0"))));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

/* @inline */
static inline SageValue sage_fn_to_oct_10(SageValue arg0) {
    SageSlot sage_local_val_59 = sage_slot_undefined();
    SageSlot sage_local_result_58 = sage_slot_undefined();
    SageSlot sage_param_n_57 = sage_slot_undefined();
    SageSlot* sage_gc_roots[3] = {&sage_local_val_59, &sage_local_result_58, &sage_param_n_57};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 3);
    sage_define_slot(&sage_param_n_57, arg0);
    sage_define_slot(&sage_local_result_58, sage_string(""));
    sage_define_slot(&sage_local_val_59, sage_load_slot(&sage_param_n_57, "n"));
    if (sage_truthy(sage_eq(sage_load_slot(&sage_local_val_59, "val"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_result_58, "result", sage_string("0"));
    }
    while (sage_truthy(sage_gt(sage_load_slot(&sage_local_val_59, "val"), sage_number(0)))) {
        (void)sage_assign_slot(&sage_local_result_58, "result", sage_add(sage_str(sage_bit_and(sage_load_slot(&sage_local_val_59, "val"), sage_number(7))), sage_load_slot(&sage_local_result_58, "result")));
        (void)sage_assign_slot(&sage_local_val_59, "val", sage_rshift(sage_load_slot(&sage_local_val_59, "val"), sage_number(3)));
    }
    return sage_gc_return(&sage_gc_frame, sage_add(sage_string("0o"), sage_load_slot(&sage_local_result_58, "result")));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_template_11(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_key_64 = sage_slot_undefined();
    SageSlot sage_local_i_63 = sage_slot_undefined();
    SageSlot sage_local_result_62 = sage_slot_undefined();
    SageSlot sage_param_values_61 = sage_slot_undefined();
    SageSlot sage_param_tmpl_60 = sage_slot_undefined();
    SageSlot* sage_gc_roots[5] = {&sage_local_key_64, &sage_local_i_63, &sage_local_result_62, &sage_param_values_61, &sage_param_tmpl_60};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 5);
    sage_define_slot(&sage_param_tmpl_60, arg0);
    sage_define_slot(&sage_param_values_61, arg1);
    sage_define_slot(&sage_local_result_62, sage_string(""));
    sage_define_slot(&sage_local_i_63, sage_number(0));
    while (sage_truthy(sage_lt(sage_load_slot(&sage_local_i_63, "i"), sage_len(sage_load_slot(&sage_param_tmpl_60, "tmpl"))))) {
        if (sage_truthy(sage_eq(sage_index(sage_load_slot(&sage_param_tmpl_60, "tmpl"), sage_load_slot(&sage_local_i_63, "i")), sage_string("{")))) {
            sage_define_slot(&sage_local_key_64, sage_string(""));
            (void)sage_assign_slot(&sage_local_i_63, "i", sage_add(sage_load_slot(&sage_local_i_63, "i"), sage_number(1)));
            while (sage_truthy(sage_and(sage_lt(sage_load_slot(&sage_local_i_63, "i"), sage_len(sage_load_slot(&sage_param_tmpl_60, "tmpl"))), sage_neq(sage_index(sage_load_slot(&sage_param_tmpl_60, "tmpl"), sage_load_slot(&sage_local_i_63, "i")), sage_string("}"))))) {
                (void)sage_assign_slot(&sage_local_key_64, "key", sage_add(sage_load_slot(&sage_local_key_64, "key"), sage_index(sage_load_slot(&sage_param_tmpl_60, "tmpl"), sage_load_slot(&sage_local_i_63, "i"))));
                (void)sage_assign_slot(&sage_local_i_63, "i", sage_add(sage_load_slot(&sage_local_i_63, "i"), sage_number(1)));
            }
            if (sage_truthy(sage_lt(sage_load_slot(&sage_local_i_63, "i"), sage_len(sage_load_slot(&sage_param_tmpl_60, "tmpl"))))) {
                (void)sage_assign_slot(&sage_local_i_63, "i", sage_add(sage_load_slot(&sage_local_i_63, "i"), sage_number(1)));
            }
            if (sage_truthy(sage_dict_has_fn(sage_load_slot(&sage_param_values_61, "values"), sage_load_slot(&sage_local_key_64, "key")))) {
                (void)sage_assign_slot(&sage_local_result_62, "result", sage_add(sage_load_slot(&sage_local_result_62, "result"), sage_str(sage_index(sage_load_slot(&sage_param_values_61, "values"), sage_load_slot(&sage_local_key_64, "key")))));
            }
            else {
                (void)sage_assign_slot(&sage_local_result_62, "result", sage_add(sage_add(sage_add(sage_load_slot(&sage_local_result_62, "result"), sage_string("{")), sage_load_slot(&sage_local_key_64, "key")), sage_string("}")));
            }
        }
        else {
            (void)sage_assign_slot(&sage_local_result_62, "result", sage_add(sage_load_slot(&sage_local_result_62, "result"), sage_index(sage_load_slot(&sage_param_tmpl_60, "tmpl"), sage_load_slot(&sage_local_i_63, "i"))));
            (void)sage_assign_slot(&sage_local_i_63, "i", sage_add(sage_load_slot(&sage_local_i_63, "i"), sage_number(1)));
        }
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_62, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_join_12(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_i_68 = sage_slot_undefined();
    SageSlot sage_local_result_67 = sage_slot_undefined();
    SageSlot sage_param_sep_66 = sage_slot_undefined();
    SageSlot sage_param_arr_65 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_i_68, &sage_local_result_67, &sage_param_sep_66, &sage_param_arr_65};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_arr_65, arg0);
    sage_define_slot(&sage_param_sep_66, arg1);
    sage_define_slot(&sage_local_result_67, sage_string(""));
    {
        SageValue sage_iter_i_69 = sage_range1(sage_len(sage_load_slot(&sage_param_arr_65, "arr")));
        if (sage_iter_i_69.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_70 = 0; sage_idx_i_70 < sage_iter_i_69.as.array->count; sage_idx_i_70++) {
                sage_define_slot(&sage_local_i_68, sage_iter_i_69.as.array->elements[sage_idx_i_70]);
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_68, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_67, "result", sage_add(sage_load_slot(&sage_local_result_67, "result"), sage_load_slot(&sage_param_sep_66, "sep")));
                    }
                    (void)sage_assign_slot(&sage_local_result_67, "result", sage_add(sage_load_slot(&sage_local_result_67, "result"), sage_str(sage_index(sage_load_slot(&sage_param_arr_65, "arr"), sage_load_slot(&sage_local_i_68, "i")))));
            }
        } else if (sage_iter_i_69.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_69.as.string);
            for (int sage_idx_i_70 = 0; sage_idx_i_70 < _len; sage_idx_i_70++) {
                char _ch[2] = {sage_iter_i_69.as.string[sage_idx_i_70], '\0'};
                sage_define_slot(&sage_local_i_68, sage_string(_ch));
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_68, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_67, "result", sage_add(sage_load_slot(&sage_local_result_67, "result"), sage_load_slot(&sage_param_sep_66, "sep")));
                    }
                    (void)sage_assign_slot(&sage_local_result_67, "result", sage_add(sage_load_slot(&sage_local_result_67, "result"), sage_str(sage_index(sage_load_slot(&sage_param_arr_65, "arr"), sage_load_slot(&sage_local_i_68, "i")))));
            }
        }
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_67, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_repeat_char_13(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_i_74 = sage_slot_undefined();
    SageSlot sage_local_result_73 = sage_slot_undefined();
    SageSlot sage_param_n_72 = sage_slot_undefined();
    SageSlot sage_param_ch_71 = sage_slot_undefined();
    SageSlot* sage_gc_roots[4] = {&sage_local_i_74, &sage_local_result_73, &sage_param_n_72, &sage_param_ch_71};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 4);
    sage_define_slot(&sage_param_ch_71, arg0);
    sage_define_slot(&sage_param_n_72, arg1);
    sage_define_slot(&sage_local_result_73, sage_string(""));
    {
        SageValue sage_iter_i_75 = sage_range1(sage_load_slot(&sage_param_n_72, "n"));
        if (sage_iter_i_75.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_76 = 0; sage_idx_i_76 < sage_iter_i_75.as.array->count; sage_idx_i_76++) {
                sage_define_slot(&sage_local_i_74, sage_iter_i_75.as.array->elements[sage_idx_i_76]);
                    (void)sage_assign_slot(&sage_local_result_73, "result", sage_add(sage_load_slot(&sage_local_result_73, "result"), sage_load_slot(&sage_param_ch_71, "ch")));
            }
        } else if (sage_iter_i_75.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_75.as.string);
            for (int sage_idx_i_76 = 0; sage_idx_i_76 < _len; sage_idx_i_76++) {
                char _ch[2] = {sage_iter_i_75.as.string[sage_idx_i_76], '\0'};
                sage_define_slot(&sage_local_i_74, sage_string(_ch));
                    (void)sage_assign_slot(&sage_local_result_73, "result", sage_add(sage_load_slot(&sage_local_result_73, "result"), sage_load_slot(&sage_param_ch_71, "ch")));
            }
        }
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_73, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

static SageValue sage_fn_table_14(SageValue arg0, SageValue arg1) {
    SageSlot sage_local_result_85 = sage_slot_undefined();
    SageSlot sage_local_w_84 = sage_slot_undefined();
    SageSlot sage_local_c_83 = sage_slot_undefined();
    SageSlot sage_local_r_82 = sage_slot_undefined();
    SageSlot sage_local_i_81 = sage_slot_undefined();
    SageSlot sage_local_widths_80 = sage_slot_undefined();
    SageSlot sage_local_nl_79 = sage_slot_undefined();
    SageSlot sage_param_rows_78 = sage_slot_undefined();
    SageSlot sage_param_headers_77 = sage_slot_undefined();
    SageSlot* sage_gc_roots[9] = {&sage_local_result_85, &sage_local_w_84, &sage_local_c_83, &sage_local_r_82, &sage_local_i_81, &sage_local_widths_80, &sage_local_nl_79, &sage_param_rows_78, &sage_param_headers_77};
    SageGcFrame sage_gc_frame;
    sage_gc_push_frame(&sage_gc_frame, sage_gc_roots, 9);
    sage_define_slot(&sage_param_headers_77, arg0);
    sage_define_slot(&sage_param_rows_78, arg1);
    sage_define_slot(&sage_local_nl_79, sage_chr(sage_number(10)));
    sage_define_slot(&sage_local_widths_80, sage_make_array(0, NULL));
    {
        SageValue sage_iter_i_86 = sage_range1(sage_len(sage_load_slot(&sage_param_headers_77, "headers")));
        if (sage_iter_i_86.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_87 = 0; sage_idx_i_87 < sage_iter_i_86.as.array->count; sage_idx_i_87++) {
                sage_define_slot(&sage_local_i_81, sage_iter_i_86.as.array->elements[sage_idx_i_87]);
                    (void)sage_push(sage_load_slot(&sage_local_widths_80, "widths"), sage_len(sage_str(sage_index(sage_load_slot(&sage_param_headers_77, "headers"), sage_load_slot(&sage_local_i_81, "i")))));
            }
        } else if (sage_iter_i_86.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_86.as.string);
            for (int sage_idx_i_87 = 0; sage_idx_i_87 < _len; sage_idx_i_87++) {
                char _ch[2] = {sage_iter_i_86.as.string[sage_idx_i_87], '\0'};
                sage_define_slot(&sage_local_i_81, sage_string(_ch));
                    (void)sage_push(sage_load_slot(&sage_local_widths_80, "widths"), sage_len(sage_str(sage_index(sage_load_slot(&sage_param_headers_77, "headers"), sage_load_slot(&sage_local_i_81, "i")))));
            }
        }
    }
    {
        SageValue sage_iter_r_88 = sage_range1(sage_len(sage_load_slot(&sage_param_rows_78, "rows")));
        if (sage_iter_r_88.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_r_89 = 0; sage_idx_r_89 < sage_iter_r_88.as.array->count; sage_idx_r_89++) {
                sage_define_slot(&sage_local_r_82, sage_iter_r_88.as.array->elements[sage_idx_r_89]);
                    {
                        SageValue sage_iter_c_90 = sage_range1(sage_len(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r"))));
                        if (sage_iter_c_90.type == SAGE_TAG_ARRAY) {
                            for (int sage_idx_c_91 = 0; sage_idx_c_91 < sage_iter_c_90.as.array->count; sage_idx_c_91++) {
                                sage_define_slot(&sage_local_c_83, sage_iter_c_90.as.array->elements[sage_idx_c_91]);
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        sage_define_slot(&sage_local_w_84, sage_len(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c")))));
                                        if (sage_truthy(sage_gt(sage_load_slot(&sage_local_w_84, "w"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"))))) {
                                            (void)sage_index_set(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"), sage_load_slot(&sage_local_w_84, "w"));
                                        }
                                    }
                            }
                        } else if (sage_iter_c_90.type == SAGE_TAG_STRING) {
                            int _len = (int)strlen(sage_iter_c_90.as.string);
                            for (int sage_idx_c_91 = 0; sage_idx_c_91 < _len; sage_idx_c_91++) {
                                char _ch[2] = {sage_iter_c_90.as.string[sage_idx_c_91], '\0'};
                                sage_define_slot(&sage_local_c_83, sage_string(_ch));
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        sage_define_slot(&sage_local_w_84, sage_len(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c")))));
                                        if (sage_truthy(sage_gt(sage_load_slot(&sage_local_w_84, "w"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"))))) {
                                            (void)sage_index_set(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"), sage_load_slot(&sage_local_w_84, "w"));
                                        }
                                    }
                            }
                        }
                    }
            }
        } else if (sage_iter_r_88.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_r_88.as.string);
            for (int sage_idx_r_89 = 0; sage_idx_r_89 < _len; sage_idx_r_89++) {
                char _ch[2] = {sage_iter_r_88.as.string[sage_idx_r_89], '\0'};
                sage_define_slot(&sage_local_r_82, sage_string(_ch));
                    {
                        SageValue sage_iter_c_92 = sage_range1(sage_len(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r"))));
                        if (sage_iter_c_92.type == SAGE_TAG_ARRAY) {
                            for (int sage_idx_c_93 = 0; sage_idx_c_93 < sage_iter_c_92.as.array->count; sage_idx_c_93++) {
                                sage_define_slot(&sage_local_c_83, sage_iter_c_92.as.array->elements[sage_idx_c_93]);
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        sage_define_slot(&sage_local_w_84, sage_len(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c")))));
                                        if (sage_truthy(sage_gt(sage_load_slot(&sage_local_w_84, "w"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"))))) {
                                            (void)sage_index_set(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"), sage_load_slot(&sage_local_w_84, "w"));
                                        }
                                    }
                            }
                        } else if (sage_iter_c_92.type == SAGE_TAG_STRING) {
                            int _len = (int)strlen(sage_iter_c_92.as.string);
                            for (int sage_idx_c_93 = 0; sage_idx_c_93 < _len; sage_idx_c_93++) {
                                char _ch[2] = {sage_iter_c_92.as.string[sage_idx_c_93], '\0'};
                                sage_define_slot(&sage_local_c_83, sage_string(_ch));
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        sage_define_slot(&sage_local_w_84, sage_len(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c")))));
                                        if (sage_truthy(sage_gt(sage_load_slot(&sage_local_w_84, "w"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"))))) {
                                            (void)sage_index_set(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c"), sage_load_slot(&sage_local_w_84, "w"));
                                        }
                                    }
                            }
                        }
                    }
            }
        }
    }
    sage_define_slot(&sage_local_result_85, sage_string(""));
    {
        SageValue sage_iter_i_94 = sage_range1(sage_len(sage_load_slot(&sage_param_headers_77, "headers")));
        if (sage_iter_i_94.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_95 = 0; sage_idx_i_95 < sage_iter_i_94.as.array->count; sage_idx_i_95++) {
                sage_define_slot(&sage_local_i_81, sage_iter_i_94.as.array->elements[sage_idx_i_95]);
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_81, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_load_slot(&sage_param_headers_77, "headers"), sage_load_slot(&sage_local_i_81, "i"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_i_81, "i")), sage_string(" "))));
            }
        } else if (sage_iter_i_94.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_94.as.string);
            for (int sage_idx_i_95 = 0; sage_idx_i_95 < _len; sage_idx_i_95++) {
                char _ch[2] = {sage_iter_i_94.as.string[sage_idx_i_95], '\0'};
                sage_define_slot(&sage_local_i_81, sage_string(_ch));
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_81, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_load_slot(&sage_param_headers_77, "headers"), sage_load_slot(&sage_local_i_81, "i"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_i_81, "i")), sage_string(" "))));
            }
        }
    }
    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_load_slot(&sage_local_nl_79, "nl")));
    {
        SageValue sage_iter_i_96 = sage_range1(sage_len(sage_load_slot(&sage_param_headers_77, "headers")));
        if (sage_iter_i_96.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_i_97 = 0; sage_idx_i_97 < sage_iter_i_96.as.array->count; sage_idx_i_97++) {
                sage_define_slot(&sage_local_i_81, sage_iter_i_96.as.array->elements[sage_idx_i_97]);
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_81, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string("-+-")));
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_repeat_char_13(sage_string("-"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_i_81, "i")))));
            }
        } else if (sage_iter_i_96.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_i_96.as.string);
            for (int sage_idx_i_97 = 0; sage_idx_i_97 < _len; sage_idx_i_97++) {
                char _ch[2] = {sage_iter_i_96.as.string[sage_idx_i_97], '\0'};
                sage_define_slot(&sage_local_i_81, sage_string(_ch));
                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_i_81, "i"), sage_number(0)))) {
                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string("-+-")));
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_repeat_char_13(sage_string("-"), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_i_81, "i")))));
            }
        }
    }
    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_load_slot(&sage_local_nl_79, "nl")));
    {
        SageValue sage_iter_r_98 = sage_range1(sage_len(sage_load_slot(&sage_param_rows_78, "rows")));
        if (sage_iter_r_98.type == SAGE_TAG_ARRAY) {
            for (int sage_idx_r_99 = 0; sage_idx_r_99 < sage_iter_r_98.as.array->count; sage_idx_r_99++) {
                sage_define_slot(&sage_local_r_82, sage_iter_r_98.as.array->elements[sage_idx_r_99]);
                    {
                        SageValue sage_iter_c_100 = sage_range1(sage_len(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r"))));
                        if (sage_iter_c_100.type == SAGE_TAG_ARRAY) {
                            for (int sage_idx_c_101 = 0; sage_idx_c_101 < sage_iter_c_100.as.array->count; sage_idx_c_101++) {
                                sage_define_slot(&sage_local_c_83, sage_iter_c_100.as.array->elements[sage_idx_c_101]);
                                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_c_83, "c"), sage_number(0)))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                                    }
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c")), sage_string(" "))));
                                    }
                            }
                        } else if (sage_iter_c_100.type == SAGE_TAG_STRING) {
                            int _len = (int)strlen(sage_iter_c_100.as.string);
                            for (int sage_idx_c_101 = 0; sage_idx_c_101 < _len; sage_idx_c_101++) {
                                char _ch[2] = {sage_iter_c_100.as.string[sage_idx_c_101], '\0'};
                                sage_define_slot(&sage_local_c_83, sage_string(_ch));
                                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_c_83, "c"), sage_number(0)))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                                    }
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c")), sage_string(" "))));
                                    }
                            }
                        }
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_load_slot(&sage_local_nl_79, "nl")));
            }
        } else if (sage_iter_r_98.type == SAGE_TAG_STRING) {
            int _len = (int)strlen(sage_iter_r_98.as.string);
            for (int sage_idx_r_99 = 0; sage_idx_r_99 < _len; sage_idx_r_99++) {
                char _ch[2] = {sage_iter_r_98.as.string[sage_idx_r_99], '\0'};
                sage_define_slot(&sage_local_r_82, sage_string(_ch));
                    {
                        SageValue sage_iter_c_102 = sage_range1(sage_len(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r"))));
                        if (sage_iter_c_102.type == SAGE_TAG_ARRAY) {
                            for (int sage_idx_c_103 = 0; sage_idx_c_103 < sage_iter_c_102.as.array->count; sage_idx_c_103++) {
                                sage_define_slot(&sage_local_c_83, sage_iter_c_102.as.array->elements[sage_idx_c_103]);
                                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_c_83, "c"), sage_number(0)))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                                    }
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c")), sage_string(" "))));
                                    }
                            }
                        } else if (sage_iter_c_102.type == SAGE_TAG_STRING) {
                            int _len = (int)strlen(sage_iter_c_102.as.string);
                            for (int sage_idx_c_103 = 0; sage_idx_c_103 < _len; sage_idx_c_103++) {
                                char _ch[2] = {sage_iter_c_102.as.string[sage_idx_c_103], '\0'};
                                sage_define_slot(&sage_local_c_83, sage_string(_ch));
                                    if (sage_truthy(sage_gt(sage_load_slot(&sage_local_c_83, "c"), sage_number(0)))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_string(" | ")));
                                    }
                                    if (sage_truthy(sage_lt(sage_load_slot(&sage_local_c_83, "c"), sage_len(sage_load_slot(&sage_local_widths_80, "widths"))))) {
                                        (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_fn_pad_right_2(sage_str(sage_index(sage_index(sage_load_slot(&sage_param_rows_78, "rows"), sage_load_slot(&sage_local_r_82, "r")), sage_load_slot(&sage_local_c_83, "c"))), sage_index(sage_load_slot(&sage_local_widths_80, "widths"), sage_load_slot(&sage_local_c_83, "c")), sage_string(" "))));
                                    }
                            }
                        }
                    }
                    (void)sage_assign_slot(&sage_local_result_85, "result", sage_add(sage_load_slot(&sage_local_result_85, "result"), sage_load_slot(&sage_local_nl_79, "nl")));
            }
        }
    }
    return sage_gc_return(&sage_gc_frame, sage_load_slot(&sage_local_result_85, "result"));
    return sage_gc_return(&sage_gc_frame, sage_nil());
}

int main(void) {
    SageGcFrame sage_gc_main_frame;
    sage_gc_push_frame(&sage_gc_main_frame, NULL, 0);
    (void)sage_gc_disable_fn();
    sage_print_ln(sage_call_method(sage_nil(), "format_int", 1, (SageValue[]){sage_number(42)}));
return 1;
}
