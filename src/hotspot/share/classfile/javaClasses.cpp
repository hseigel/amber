/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/altHashing.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/debugInfo.hpp"
#include "code/dependencyContext.hpp"
#include "code/pcDesc.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/linkResolver.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/heapShared.inline.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/fieldStreams.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/klass.hpp"
#include "oops/method.inline.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "oops/recordComponent.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/resolvedMethodTable.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/align.hpp"
#include "utilities/preserveException.hpp"
#include "utilities/utf8.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmciJavaClasses.hpp"
#endif

#define INJECTED_FIELD_COMPUTE_OFFSET(klass, name, signature, may_be_java)    \
  klass::_##name##_offset = JavaClasses::compute_injected_offset(JavaClasses::klass##_##name##_enum);

#if INCLUDE_CDS
#define INJECTED_FIELD_SERIALIZE_OFFSET(klass, name, signature, may_be_java) \
  f->do_u4((u4*)&_##name##_offset);
#endif

#define DECLARE_INJECTED_FIELD(klass, name, signature, may_be_java)           \
  { SystemDictionary::WK_KLASS_ENUM_NAME(klass), vmSymbols::VM_SYMBOL_ENUM_NAME(name##_name), vmSymbols::VM_SYMBOL_ENUM_NAME(signature), may_be_java },

InjectedField JavaClasses::_injected_fields[] = {
  ALL_INJECTED_FIELDS(DECLARE_INJECTED_FIELD)
};

int JavaClasses::compute_injected_offset(InjectedFieldID id) {
  return _injected_fields[id].compute_offset();
}

InjectedField* JavaClasses::get_injected(Symbol* class_name, int* field_count) {
  *field_count = 0;

  vmSymbols::SID sid = vmSymbols::find_sid(class_name);
  if (sid == vmSymbols::NO_SID) {
    // Only well known classes can inject fields
    return NULL;
  }

  int count = 0;
  int start = -1;

#define LOOKUP_INJECTED_FIELD(klass, name, signature, may_be_java) \
  if (sid == vmSymbols::VM_SYMBOL_ENUM_NAME(klass)) {              \
    count++;                                                       \
    if (start == -1) start = klass##_##name##_enum;                \
  }
  ALL_INJECTED_FIELDS(LOOKUP_INJECTED_FIELD);
#undef LOOKUP_INJECTED_FIELD

  if (start != -1) {
    *field_count = count;
    return _injected_fields + start;
  }
  return NULL;
}


// Helpful routine for computing field offsets at run time rather than hardcoding them
// Finds local fields only, including static fields.  Static field offsets are from the
// beginning of the mirror.
static void compute_offset(int &dest_offset,
                           InstanceKlass* ik, Symbol* name_symbol, Symbol* signature_symbol,
                           bool is_static = false) {
  fieldDescriptor fd;
  if (ik == NULL) {
    ResourceMark rm;
    log_error(class)("Mismatch JDK version for field: %s type: %s", name_symbol->as_C_string(), signature_symbol->as_C_string());
    vm_exit_during_initialization("Invalid layout of well-known class");
  }

  if (!ik->find_local_field(name_symbol, signature_symbol, &fd) || fd.is_static() != is_static) {
    ResourceMark rm;
    log_error(class)("Invalid layout of %s field: %s type: %s", ik->external_name(),
                     name_symbol->as_C_string(), signature_symbol->as_C_string());
#ifndef PRODUCT
    // Prints all fields and offsets
    Log(class) lt;
    LogStream ls(lt.error());
    ik->print_on(&ls);
#endif //PRODUCT
    vm_exit_during_initialization("Invalid layout of well-known class: use -Xlog:class+load=info to see the origin of the problem class");
  }
  dest_offset = fd.offset();
}

// Overloading to pass name as a string.
static void compute_offset(int& dest_offset, InstanceKlass* ik,
                           const char* name_string, Symbol* signature_symbol,
                           bool is_static = false) {
  TempNewSymbol name = SymbolTable::probe(name_string, (int)strlen(name_string));
  if (name == NULL) {
    ResourceMark rm;
    log_error(class)("Name %s should be in the SymbolTable since its class is loaded", name_string);
    vm_exit_during_initialization("Invalid layout of well-known class", ik->external_name());
  }
  compute_offset(dest_offset, ik, name, signature_symbol, is_static);
}

int java_lang_String::value_offset  = 0;
int java_lang_String::hash_offset   = 0;
int java_lang_String::hashIsZero_offset = 0;
int java_lang_String::coder_offset  = 0;

bool java_lang_String::initialized  = false;

bool java_lang_String::is_instance(oop obj) {
  return is_instance_inlined(obj);
}

#if INCLUDE_CDS
#define FIELD_SERIALIZE_OFFSET(offset, klass, name, signature, is_static) \
  f->do_u4((u4*)&offset)
#endif

#define FIELD_COMPUTE_OFFSET(offset, klass, name, signature, is_static) \
  compute_offset(offset, klass, name, vmSymbols::signature(), is_static)

#define STRING_FIELDS_DO(macro) \
  macro(value_offset, k, vmSymbols::value_name(), byte_array_signature, false); \
  macro(hash_offset,  k, "hash",                  int_signature,        false); \
  macro(hashIsZero_offset, k, "hashIsZero",       bool_signature,       false); \
  macro(coder_offset, k, "coder",                 byte_signature,       false);

void java_lang_String::compute_offsets() {
  if (initialized) {
    return;
  }

  InstanceKlass* k = SystemDictionary::String_klass();
  STRING_FIELDS_DO(FIELD_COMPUTE_OFFSET);

  initialized = true;
}

#if INCLUDE_CDS
void java_lang_String::serialize_offsets(SerializeClosure* f) {
  STRING_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
  f->do_bool(&initialized);
}
#endif

class CompactStringsFixup : public FieldClosure {
private:
  bool _value;

public:
  CompactStringsFixup(bool value) : _value(value) {}

  void do_field(fieldDescriptor* fd) {
    if (fd->name() == vmSymbols::compact_strings_name()) {
      oop mirror = fd->field_holder()->java_mirror();
      assert(fd->field_holder() == SystemDictionary::String_klass(), "Should be String");
      assert(mirror != NULL, "String must have mirror already");
      mirror->bool_field_put(fd->offset(), _value);
    }
  }
};

void java_lang_String::set_compact_strings(bool value) {
  CompactStringsFixup fix(value);
  SystemDictionary::String_klass()->do_local_static_fields(&fix);
}

Handle java_lang_String::basic_create(int length, bool is_latin1, TRAPS) {
  assert(initialized, "Must be initialized");
  assert(CompactStrings || !is_latin1, "Must be UTF16 without CompactStrings");

  // Create the String object first, so there's a chance that the String
  // and the char array it points to end up in the same cache line.
  oop obj;
  obj = SystemDictionary::String_klass()->allocate_instance(CHECK_NH);

  // Create the char array.  The String object must be handlized here
  // because GC can happen as a result of the allocation attempt.
  Handle h_obj(THREAD, obj);
  int arr_length = is_latin1 ? length : length << 1; // 2 bytes per UTF16.
  typeArrayOop buffer = oopFactory::new_byteArray(arr_length, CHECK_NH);;

  // Point the String at the char array
  obj = h_obj();
  set_value(obj, buffer);
  // No need to zero the offset, allocation zero'ed the entire String object
  set_coder(obj, is_latin1 ? CODER_LATIN1 : CODER_UTF16);
  return h_obj;
}

Handle java_lang_String::create_from_unicode(const jchar* unicode, int length, TRAPS) {
  bool is_latin1 = CompactStrings && UNICODE::is_latin1(unicode, length);
  Handle h_obj = basic_create(length, is_latin1, CHECK_NH);
  typeArrayOop buffer = value(h_obj());
  assert(TypeArrayKlass::cast(buffer->klass())->element_type() == T_BYTE, "only byte[]");
  if (is_latin1) {
    for (int index = 0; index < length; index++) {
      buffer->byte_at_put(index, (jbyte)unicode[index]);
    }
  } else {
    for (int index = 0; index < length; index++) {
      buffer->char_at_put(index, unicode[index]);
    }
  }

#ifdef ASSERT
  {
    ResourceMark rm;
    char* expected = UNICODE::as_utf8(unicode, length);
    char* actual = as_utf8_string(h_obj());
    if (strcmp(expected, actual) != 0) {
      tty->print_cr("Unicode conversion failure: %s --> %s", expected, actual);
      ShouldNotReachHere();
    }
  }
#endif

  return h_obj;
}

oop java_lang_String::create_oop_from_unicode(const jchar* unicode, int length, TRAPS) {
  Handle h_obj = create_from_unicode(unicode, length, CHECK_0);
  return h_obj();
}

Handle java_lang_String::create_from_str(const char* utf8_str, TRAPS) {
  if (utf8_str == NULL) {
    return Handle();
  }
  bool has_multibyte, is_latin1;
  int length = UTF8::unicode_length(utf8_str, is_latin1, has_multibyte);
  if (!CompactStrings) {
    has_multibyte = true;
    is_latin1 = false;
  }

  Handle h_obj = basic_create(length, is_latin1, CHECK_NH);
  if (length > 0) {
    if (!has_multibyte) {
      const jbyte* src = reinterpret_cast<const jbyte*>(utf8_str);
      ArrayAccess<>::arraycopy_from_native(src, value(h_obj()), typeArrayOopDesc::element_offset<jbyte>(0), length);
    } else if (is_latin1) {
      UTF8::convert_to_unicode(utf8_str, value(h_obj())->byte_at_addr(0), length);
    } else {
      UTF8::convert_to_unicode(utf8_str, value(h_obj())->char_at_addr(0), length);
    }
  }

#ifdef ASSERT
  // This check is too strict because the input string is not necessarily valid UTF8.
  // For example, it may be created with arbitrary content via jni_NewStringUTF.
  /*
  {
    ResourceMark rm;
    const char* expected = utf8_str;
    char* actual = as_utf8_string(h_obj());
    if (strcmp(expected, actual) != 0) {
      tty->print_cr("String conversion failure: %s --> %s", expected, actual);
      ShouldNotReachHere();
    }
  }
  */
#endif

  return h_obj;
}

oop java_lang_String::create_oop_from_str(const char* utf8_str, TRAPS) {
  Handle h_obj = create_from_str(utf8_str, CHECK_0);
  return h_obj();
}

Handle java_lang_String::create_from_symbol(Symbol* symbol, TRAPS) {
  const char* utf8_str = (char*)symbol->bytes();
  int utf8_len = symbol->utf8_length();

  bool has_multibyte, is_latin1;
  int length = UTF8::unicode_length(utf8_str, utf8_len, is_latin1, has_multibyte);
  if (!CompactStrings) {
    has_multibyte = true;
    is_latin1 = false;
  }

  Handle h_obj = basic_create(length, is_latin1, CHECK_NH);
  if (length > 0) {
    if (!has_multibyte) {
      const jbyte* src = reinterpret_cast<const jbyte*>(utf8_str);
      ArrayAccess<>::arraycopy_from_native(src, value(h_obj()), typeArrayOopDesc::element_offset<jbyte>(0), length);
    } else if (is_latin1) {
      UTF8::convert_to_unicode(utf8_str, value(h_obj())->byte_at_addr(0), length);
    } else {
      UTF8::convert_to_unicode(utf8_str, value(h_obj())->char_at_addr(0), length);
    }
  }

#ifdef ASSERT
  {
    ResourceMark rm;
    const char* expected = symbol->as_utf8();
    char* actual = as_utf8_string(h_obj());
    if (strncmp(expected, actual, utf8_len) != 0) {
      tty->print_cr("Symbol conversion failure: %s --> %s", expected, actual);
      ShouldNotReachHere();
    }
  }
#endif

  return h_obj;
}

// Converts a C string to a Java String based on current encoding
Handle java_lang_String::create_from_platform_dependent_str(const char* str, TRAPS) {
  assert(str != NULL, "bad arguments");

  typedef jstring (*to_java_string_fn_t)(JNIEnv*, const char *);
  static to_java_string_fn_t _to_java_string_fn = NULL;

  if (_to_java_string_fn == NULL) {
    void *lib_handle = os::native_java_library();
    _to_java_string_fn = CAST_TO_FN_PTR(to_java_string_fn_t, os::dll_lookup(lib_handle, "NewStringPlatform"));
    if (_to_java_string_fn == NULL) {
      fatal("NewStringPlatform missing");
    }
  }

  jstring js = NULL;
  {
    assert(THREAD->is_Java_thread(), "must be java thread");
    JavaThread* thread = (JavaThread*)THREAD;
    HandleMark hm(thread);
    ThreadToNativeFromVM ttn(thread);
    js = (_to_java_string_fn)(thread->jni_environment(), str);
  }

  Handle native_platform_string(THREAD, JNIHandles::resolve(js));
  JNIHandles::destroy_local(js);  // destroy local JNIHandle.
  return native_platform_string;
}

// Converts a Java String to a native C string that can be used for
// native OS calls.
char* java_lang_String::as_platform_dependent_str(Handle java_string, TRAPS) {
  typedef char* (*to_platform_string_fn_t)(JNIEnv*, jstring, bool*);
  static to_platform_string_fn_t _to_platform_string_fn = NULL;

  if (_to_platform_string_fn == NULL) {
    void *lib_handle = os::native_java_library();
    _to_platform_string_fn = CAST_TO_FN_PTR(to_platform_string_fn_t, os::dll_lookup(lib_handle, "GetStringPlatformChars"));
    if (_to_platform_string_fn == NULL) {
      fatal("GetStringPlatformChars missing");
    }
  }

  char *native_platform_string;
  { JavaThread* thread = (JavaThread*)THREAD;
    assert(thread->is_Java_thread(), "must be java thread");
    JNIEnv *env = thread->jni_environment();
    jstring js = (jstring) JNIHandles::make_local(env, java_string());
    bool is_copy;
    HandleMark hm(thread);
    ThreadToNativeFromVM ttn(thread);
    native_platform_string = (_to_platform_string_fn)(env, js, &is_copy);
    assert(is_copy == JNI_TRUE, "is_copy value changed");
    JNIHandles::destroy_local(js);
  }
  return native_platform_string;
}

Handle java_lang_String::char_converter(Handle java_string, jchar from_char, jchar to_char, TRAPS) {
  oop          obj    = java_string();
  // Typical usage is to convert all '/' to '.' in string.
  typeArrayOop value  = java_lang_String::value(obj);
  int          length = java_lang_String::length(obj, value);
  bool      is_latin1 = java_lang_String::is_latin1(obj);

  // First check if any from_char exist
  int index; // Declared outside, used later
  for (index = 0; index < length; index++) {
    jchar c = !is_latin1 ? value->char_at(index) :
                  ((jchar) value->byte_at(index)) & 0xff;
    if (c == from_char) {
      break;
    }
  }
  if (index == length) {
    // No from_char, so do not copy.
    return java_string;
  }

  // Check if result string will be latin1
  bool to_is_latin1 = false;

  // Replacement char must be latin1
  if (CompactStrings && UNICODE::is_latin1(to_char)) {
    if (is_latin1) {
      // Source string is latin1 as well
      to_is_latin1 = true;
    } else if (!UNICODE::is_latin1(from_char)) {
      // We are replacing an UTF16 char. Scan string to
      // check if result can be latin1 encoded.
      to_is_latin1 = true;
      for (index = 0; index < length; index++) {
        jchar c = value->char_at(index);
        if (c != from_char && !UNICODE::is_latin1(c)) {
          to_is_latin1 = false;
          break;
        }
      }
    }
  }

  // Create new UNICODE (or byte) buffer. Must handlize value because GC
  // may happen during String and char array creation.
  typeArrayHandle h_value(THREAD, value);
  Handle string = basic_create(length, to_is_latin1, CHECK_NH);
  typeArrayOop from_buffer = h_value();
  typeArrayOop to_buffer = java_lang_String::value(string());

  // Copy contents
  for (index = 0; index < length; index++) {
    jchar c = (!is_latin1) ? from_buffer->char_at(index) :
                    ((jchar) from_buffer->byte_at(index)) & 0xff;
    if (c == from_char) {
      c = to_char;
    }
    if (!to_is_latin1) {
      to_buffer->char_at_put(index, c);
    } else {
      to_buffer->byte_at_put(index, (jbyte) c);
    }
  }
  return string;
}

jchar* java_lang_String::as_unicode_string(oop java_string, int& length, TRAPS) {
  typeArrayOop value  = java_lang_String::value(java_string);
               length = java_lang_String::length(java_string, value);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);

  jchar* result = NEW_RESOURCE_ARRAY_RETURN_NULL(jchar, length);
  if (result != NULL) {
    if (!is_latin1) {
      for (int index = 0; index < length; index++) {
        result[index] = value->char_at(index);
      }
    } else {
      for (int index = 0; index < length; index++) {
        result[index] = ((jchar) value->byte_at(index)) & 0xff;
      }
    }
  } else {
    THROW_MSG_0(vmSymbols::java_lang_OutOfMemoryError(), "could not allocate Unicode string");
  }
  return result;
}

unsigned int java_lang_String::hash_code(oop java_string) {
  // The hash and hashIsZero fields are subject to a benign data race,
  // making it crucial to ensure that any observable result of the
  // calculation in this method stays correct under any possible read of
  // these fields. Necessary restrictions to allow this to be correct
  // without explicit memory fences or similar concurrency primitives is
  // that we can ever only write to one of these two fields for a given
  // String instance, and that the computation is idempotent and derived
  // from immutable state
  assert(initialized && (hash_offset > 0) && (hashIsZero_offset > 0), "Must be initialized");
  if (java_lang_String::hash_is_set(java_string)) {
    return java_string->int_field(hash_offset);
  }

  typeArrayOop value = java_lang_String::value(java_string);
  int         length = java_lang_String::length(java_string, value);
  bool     is_latin1 = java_lang_String::is_latin1(java_string);

  unsigned int hash = 0;
  if (length > 0) {
    if (is_latin1) {
      hash = java_lang_String::hash_code(value->byte_at_addr(0), length);
    } else {
      hash = java_lang_String::hash_code(value->char_at_addr(0), length);
    }
  }

  if (hash != 0) {
    java_string->int_field_put(hash_offset, hash);
  } else {
    java_string->bool_field_put(hashIsZero_offset, true);
  }
  return hash;
}

char* java_lang_String::as_quoted_ascii(oop java_string) {
  typeArrayOop value  = java_lang_String::value(java_string);
  int          length = java_lang_String::length(java_string, value);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);

  if (length == 0) return NULL;

  char* result;
  int result_length;
  if (!is_latin1) {
    jchar* base = value->char_at_addr(0);
    result_length = UNICODE::quoted_ascii_length(base, length) + 1;
    result = NEW_RESOURCE_ARRAY(char, result_length);
    UNICODE::as_quoted_ascii(base, length, result, result_length);
  } else {
    jbyte* base = value->byte_at_addr(0);
    result_length = UNICODE::quoted_ascii_length(base, length) + 1;
    result = NEW_RESOURCE_ARRAY(char, result_length);
    UNICODE::as_quoted_ascii(base, length, result, result_length);
  }
  assert(result_length >= length + 1, "must not be shorter");
  assert(result_length == (int)strlen(result) + 1, "must match");
  return result;
}

Symbol* java_lang_String::as_symbol(oop java_string) {
  typeArrayOop value  = java_lang_String::value(java_string);
  int          length = java_lang_String::length(java_string, value);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    jchar* base = (length == 0) ? NULL : value->char_at_addr(0);
    Symbol* sym = SymbolTable::new_symbol(base, length);
    return sym;
  } else {
    ResourceMark rm;
    jbyte* position = (length == 0) ? NULL : value->byte_at_addr(0);
    const char* base = UNICODE::as_utf8(position, length);
    Symbol* sym = SymbolTable::new_symbol(base, length);
    return sym;
  }
}

Symbol* java_lang_String::as_symbol_or_null(oop java_string) {
  typeArrayOop value  = java_lang_String::value(java_string);
  int          length = java_lang_String::length(java_string, value);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    jchar* base = (length == 0) ? NULL : value->char_at_addr(0);
    return SymbolTable::probe_unicode(base, length);
  } else {
    ResourceMark rm;
    jbyte* position = (length == 0) ? NULL : value->byte_at_addr(0);
    const char* base = UNICODE::as_utf8(position, length);
    return SymbolTable::probe(base, length);
  }
}

int java_lang_String::utf8_length(oop java_string, typeArrayOop value) {
  assert(value_equals(value, java_lang_String::value(java_string)),
         "value must be same as java_lang_String::value(java_string)");
  int length = java_lang_String::length(java_string, value);
  if (length == 0) {
    return 0;
  }
  if (!java_lang_String::is_latin1(java_string)) {
    return UNICODE::utf8_length(value->char_at_addr(0), length);
  } else {
    return UNICODE::utf8_length(value->byte_at_addr(0), length);
  }
}

int java_lang_String::utf8_length(oop java_string) {
  typeArrayOop value = java_lang_String::value(java_string);
  return utf8_length(java_string, value);
}

char* java_lang_String::as_utf8_string(oop java_string) {
  typeArrayOop value  = java_lang_String::value(java_string);
  int          length = java_lang_String::length(java_string, value);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    jchar* position = (length == 0) ? NULL : value->char_at_addr(0);
    return UNICODE::as_utf8(position, length);
  } else {
    jbyte* position = (length == 0) ? NULL : value->byte_at_addr(0);
    return UNICODE::as_utf8(position, length);
  }
}

char* java_lang_String::as_utf8_string(oop java_string, typeArrayOop value, char* buf, int buflen) {
  assert(value_equals(value, java_lang_String::value(java_string)),
         "value must be same as java_lang_String::value(java_string)");
  int     length = java_lang_String::length(java_string, value);
  bool is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    jchar* position = (length == 0) ? NULL : value->char_at_addr(0);
    return UNICODE::as_utf8(position, length, buf, buflen);
  } else {
    jbyte* position = (length == 0) ? NULL : value->byte_at_addr(0);
    return UNICODE::as_utf8(position, length, buf, buflen);
  }
}

char* java_lang_String::as_utf8_string(oop java_string, char* buf, int buflen) {
  typeArrayOop value = java_lang_String::value(java_string);
  return as_utf8_string(java_string, value, buf, buflen);
}

char* java_lang_String::as_utf8_string(oop java_string, int start, int len) {
  typeArrayOop value  = java_lang_String::value(java_string);
  bool      is_latin1 = java_lang_String::is_latin1(java_string);
  assert(start + len <= java_lang_String::length(java_string), "just checking");
  if (!is_latin1) {
    jchar* position = value->char_at_addr(start);
    return UNICODE::as_utf8(position, len);
  } else {
    jbyte* position = value->byte_at_addr(start);
    return UNICODE::as_utf8(position, len);
  }
}

char* java_lang_String::as_utf8_string(oop java_string, typeArrayOop value, int start, int len, char* buf, int buflen) {
  assert(value_equals(value, java_lang_String::value(java_string)),
         "value must be same as java_lang_String::value(java_string)");
  assert(start + len <= java_lang_String::length(java_string), "just checking");
  bool is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    jchar* position = value->char_at_addr(start);
    return UNICODE::as_utf8(position, len, buf, buflen);
  } else {
    jbyte* position = value->byte_at_addr(start);
    return UNICODE::as_utf8(position, len, buf, buflen);
  }
}

bool java_lang_String::equals(oop java_string, const jchar* chars, int len) {
  assert(java_string->klass() == SystemDictionary::String_klass(),
         "must be java_string");
  typeArrayOop value = java_lang_String::value_no_keepalive(java_string);
  int length = java_lang_String::length(java_string, value);
  if (length != len) {
    return false;
  }
  bool is_latin1 = java_lang_String::is_latin1(java_string);
  if (!is_latin1) {
    for (int i = 0; i < len; i++) {
      if (value->char_at(i) != chars[i]) {
        return false;
      }
    }
  } else {
    for (int i = 0; i < len; i++) {
      if ((((jchar) value->byte_at(i)) & 0xff) != chars[i]) {
        return false;
      }
    }
  }
  return true;
}

bool java_lang_String::equals(oop str1, oop str2) {
  assert(str1->klass() == SystemDictionary::String_klass(),
         "must be java String");
  assert(str2->klass() == SystemDictionary::String_klass(),
         "must be java String");
  typeArrayOop value1    = java_lang_String::value_no_keepalive(str1);
  bool         is_latin1 = java_lang_String::is_latin1(str1);
  typeArrayOop value2    = java_lang_String::value_no_keepalive(str2);
  bool         is_latin2 = java_lang_String::is_latin1(str2);

  if (is_latin1 != is_latin2) {
    // Strings with different coders are never equal.
    return false;
  }
  return value_equals(value1, value2);
}

void java_lang_String::print(oop java_string, outputStream* st) {
  assert(java_string->klass() == SystemDictionary::String_klass(), "must be java_string");
  typeArrayOop value  = java_lang_String::value_no_keepalive(java_string);

  if (value == NULL) {
    // This can happen if, e.g., printing a String
    // object before its initializer has been called
    st->print("NULL");
    return;
  }

  int length = java_lang_String::length(java_string, value);
  bool is_latin1 = java_lang_String::is_latin1(java_string);

  st->print("\"");
  for (int index = 0; index < length; index++) {
    st->print("%c", (!is_latin1) ?  value->char_at(index) :
                           ((jchar) value->byte_at(index)) & 0xff );
  }
  st->print("\"");
}


static void initialize_static_field(fieldDescriptor* fd, Handle mirror, TRAPS) {
  assert(mirror.not_null() && fd->is_static(), "just checking");
  if (fd->has_initial_value()) {
    BasicType t = fd->field_type();
    switch (t) {
      case T_BYTE:
        mirror()->byte_field_put(fd->offset(), fd->int_initial_value());
              break;
      case T_BOOLEAN:
        mirror()->bool_field_put(fd->offset(), fd->int_initial_value());
              break;
      case T_CHAR:
        mirror()->char_field_put(fd->offset(), fd->int_initial_value());
              break;
      case T_SHORT:
        mirror()->short_field_put(fd->offset(), fd->int_initial_value());
              break;
      case T_INT:
        mirror()->int_field_put(fd->offset(), fd->int_initial_value());
        break;
      case T_FLOAT:
        mirror()->float_field_put(fd->offset(), fd->float_initial_value());
        break;
      case T_DOUBLE:
        mirror()->double_field_put(fd->offset(), fd->double_initial_value());
        break;
      case T_LONG:
        mirror()->long_field_put(fd->offset(), fd->long_initial_value());
        break;
      case T_OBJECT:
        {
          assert(fd->signature() == vmSymbols::string_signature(),
                 "just checking");
          if (DumpSharedSpaces && HeapShared::is_archived_object(mirror())) {
            // Archive the String field and update the pointer.
            oop s = mirror()->obj_field(fd->offset());
            oop archived_s = StringTable::create_archived_string(s, CHECK);
            mirror()->obj_field_put(fd->offset(), archived_s);
          } else {
            oop string = fd->string_initial_value(CHECK);
            mirror()->obj_field_put(fd->offset(), string);
          }
        }
        break;
      default:
        THROW_MSG(vmSymbols::java_lang_ClassFormatError(),
                  "Illegal ConstantValue attribute in class file");
    }
  }
}


void java_lang_Class::fixup_mirror(Klass* k, TRAPS) {
  assert(InstanceMirrorKlass::offset_of_static_fields() != 0, "must have been computed already");

  // If the offset was read from the shared archive, it was fixed up already
  if (!k->is_shared()) {
    if (k->is_instance_klass()) {
      // During bootstrap, java.lang.Class wasn't loaded so static field
      // offsets were computed without the size added it.  Go back and
      // update all the static field offsets to included the size.
      for (JavaFieldStream fs(InstanceKlass::cast(k)); !fs.done(); fs.next()) {
        if (fs.access_flags().is_static()) {
          int real_offset = fs.offset() + InstanceMirrorKlass::offset_of_static_fields();
          fs.set_offset(real_offset);
        }
      }
    }
  }

  if (k->is_shared() && k->has_raw_archived_mirror()) {
    if (HeapShared::open_archive_heap_region_mapped()) {
      bool present = restore_archived_mirror(k, Handle(), Handle(), Handle(), CHECK);
      assert(present, "Missing archived mirror for %s", k->external_name());
      return;
    } else {
      k->set_java_mirror_handle(NULL);
      k->clear_has_raw_archived_mirror();
    }
  }
  create_mirror(k, Handle(), Handle(), Handle(), CHECK);
}

void java_lang_Class::initialize_mirror_fields(Klass* k,
                                               Handle mirror,
                                               Handle protection_domain,
                                               TRAPS) {
  // Allocate a simple java object for a lock.
  // This needs to be a java object because during class initialization
  // it can be held across a java call.
  typeArrayOop r = oopFactory::new_typeArray(T_INT, 0, CHECK);
  set_init_lock(mirror(), r);

  // Set protection domain also
  set_protection_domain(mirror(), protection_domain());

  // Initialize static fields
  InstanceKlass::cast(k)->do_local_static_fields(&initialize_static_field, mirror, CHECK);
}

// Set the java.lang.Module module field in the java_lang_Class mirror
void java_lang_Class::set_mirror_module_field(Klass* k, Handle mirror, Handle module, TRAPS) {
  if (module.is_null()) {
    // During startup, the module may be NULL only if java.base has not been defined yet.
    // Put the class on the fixup_module_list to patch later when the java.lang.Module
    // for java.base is known. But note that since we captured the NULL module another
    // thread may have completed that initialization.

    bool javabase_was_defined = false;
    {
      MutexLocker m1(Module_lock, THREAD);
      // Keep list of classes needing java.base module fixup
      if (!ModuleEntryTable::javabase_defined()) {
        assert(k->java_mirror() != NULL, "Class's mirror is null");
        k->class_loader_data()->inc_keep_alive();
        assert(fixup_module_field_list() != NULL, "fixup_module_field_list not initialized");
        fixup_module_field_list()->push(k);
      } else {
        javabase_was_defined = true;
      }
    }

    // If java.base was already defined then patch this particular class with java.base.
    if (javabase_was_defined) {
      ModuleEntry *javabase_entry = ModuleEntryTable::javabase_moduleEntry();
      assert(javabase_entry != NULL && javabase_entry->module() != NULL,
             "Setting class module field, " JAVA_BASE_NAME " should be defined");
      Handle javabase_handle(THREAD, javabase_entry->module());
      set_module(mirror(), javabase_handle());
    }
  } else {
    assert(Universe::is_module_initialized() ||
           (ModuleEntryTable::javabase_defined() &&
            (module() == ModuleEntryTable::javabase_moduleEntry()->module())),
           "Incorrect java.lang.Module specification while creating mirror");
    set_module(mirror(), module());
  }
}

// Statically allocate fixup lists because they always get created.
void java_lang_Class::allocate_fixup_lists() {
  GrowableArray<Klass*>* mirror_list =
    new (ResourceObj::C_HEAP, mtClass) GrowableArray<Klass*>(40, true);
  set_fixup_mirror_list(mirror_list);

  GrowableArray<Klass*>* module_list =
    new (ResourceObj::C_HEAP, mtModule) GrowableArray<Klass*>(500, true);
  set_fixup_module_field_list(module_list);
}

void java_lang_Class::create_mirror(Klass* k, Handle class_loader,
                                    Handle module, Handle protection_domain, TRAPS) {
  assert(k != NULL, "Use create_basic_type_mirror for primitive types");
  assert(k->java_mirror() == NULL, "should only assign mirror once");

  // Use this moment of initialization to cache modifier_flags also,
  // to support Class.getModifiers().  Instance classes recalculate
  // the cached flags after the class file is parsed, but before the
  // class is put into the system dictionary.
  int computed_modifiers = k->compute_modifier_flags(CHECK);
  k->set_modifier_flags(computed_modifiers);
  // Class_klass has to be loaded because it is used to allocate
  // the mirror.
  if (SystemDictionary::Class_klass_loaded()) {
    // Allocate mirror (java.lang.Class instance)
    oop mirror_oop = InstanceMirrorKlass::cast(SystemDictionary::Class_klass())->allocate_instance(k, CHECK);
    Handle mirror(THREAD, mirror_oop);
    Handle comp_mirror;

    // Setup indirection from mirror->klass
    java_lang_Class::set_klass(mirror(), k);

    InstanceMirrorKlass* mk = InstanceMirrorKlass::cast(mirror->klass());
    assert(oop_size(mirror()) == mk->instance_size(k), "should have been set");

    java_lang_Class::set_static_oop_field_count(mirror(), mk->compute_static_oop_field_count(mirror()));

    // It might also have a component mirror.  This mirror must already exist.
    if (k->is_array_klass()) {
      if (k->is_typeArray_klass()) {
        BasicType type = TypeArrayKlass::cast(k)->element_type();
        comp_mirror = Handle(THREAD, Universe::java_mirror(type));
      } else {
        assert(k->is_objArray_klass(), "Must be");
        Klass* element_klass = ObjArrayKlass::cast(k)->element_klass();
        assert(element_klass != NULL, "Must have an element klass");
        comp_mirror = Handle(THREAD, element_klass->java_mirror());
      }
      assert(comp_mirror() != NULL, "must have a mirror");

      // Two-way link between the array klass and its component mirror:
      // (array_klass) k -> mirror -> component_mirror -> array_klass -> k
      set_component_mirror(mirror(), comp_mirror());
      // See below for ordering dependencies between field array_klass in component mirror
      // and java_mirror in this klass.
    } else {
      assert(k->is_instance_klass(), "Must be");

      initialize_mirror_fields(k, mirror, protection_domain, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        // If any of the fields throws an exception like OOM remove the klass field
        // from the mirror so GC doesn't follow it after the klass has been deallocated.
        // This mirror looks like a primitive type, which logically it is because it
        // it represents no class.
        java_lang_Class::set_klass(mirror(), NULL);
        return;
      }
    }

    // set the classLoader field in the java_lang_Class instance
    assert(class_loader() == k->class_loader(), "should be same");
    set_class_loader(mirror(), class_loader());

    // Setup indirection from klass->mirror
    // after any exceptions can happen during allocations.
    k->set_java_mirror(mirror);

    // Set the module field in the java_lang_Class instance.  This must be done
    // after the mirror is set.
    set_mirror_module_field(k, mirror, module, THREAD);

    if (comp_mirror() != NULL) {
      // Set after k->java_mirror() is published, because compiled code running
      // concurrently doesn't expect a k to have a null java_mirror.
      release_set_array_klass(comp_mirror(), k);
    }
  } else {
    assert(fixup_mirror_list() != NULL, "fixup_mirror_list not initialized");
    fixup_mirror_list()->push(k);
  }
}

#if INCLUDE_CDS_JAVA_HEAP
// Clears mirror fields. Static final fields with initial values are reloaded
// from constant pool. The object identity hash is in the object header and is
// not affected.
class ResetMirrorField: public FieldClosure {
 private:
  Handle _m;

 public:
  ResetMirrorField(Handle mirror) : _m(mirror) {}

  void do_field(fieldDescriptor* fd) {
    assert(DumpSharedSpaces, "dump time only");
    assert(_m.not_null(), "Mirror cannot be NULL");

    if (fd->is_static() && fd->has_initial_value()) {
      initialize_static_field(fd, _m, Thread::current());
      return;
    }

    BasicType ft = fd->field_type();
    switch (ft) {
      case T_BYTE:
        _m()->byte_field_put(fd->offset(), 0);
        break;
      case T_CHAR:
        _m()->char_field_put(fd->offset(), 0);
        break;
      case T_DOUBLE:
        _m()->double_field_put(fd->offset(), 0);
        break;
      case T_FLOAT:
        _m()->float_field_put(fd->offset(), 0);
        break;
      case T_INT:
        _m()->int_field_put(fd->offset(), 0);
        break;
      case T_LONG:
        _m()->long_field_put(fd->offset(), 0);
        break;
      case T_SHORT:
        _m()->short_field_put(fd->offset(), 0);
        break;
      case T_BOOLEAN:
        _m()->bool_field_put(fd->offset(), false);
        break;
      case T_ARRAY:
      case T_OBJECT: {
        // It might be useful to cache the String field, but
        // for now just clear out any reference field
        oop o = _m()->obj_field(fd->offset());
        _m()->obj_field_put(fd->offset(), NULL);
        break;
      }
      default:
        ShouldNotReachHere();
        break;
     }
  }
};

void java_lang_Class::archive_basic_type_mirrors(TRAPS) {
  assert(HeapShared::is_heap_object_archiving_allowed(),
         "HeapShared::is_heap_object_archiving_allowed() must be true");

  for (int t = 0; t <= T_VOID; t++) {
    oop m = Universe::_mirrors[t];
    if (m != NULL) {
      // Update the field at _array_klass_offset to point to the relocated array klass.
      oop archived_m = HeapShared::archive_heap_object(m, THREAD);
      assert(archived_m != NULL, "sanity");
      Klass *ak = (Klass*)(archived_m->metadata_field(_array_klass_offset));
      assert(ak != NULL || t == T_VOID, "should not be NULL");
      if (ak != NULL) {
        Klass *reloc_ak = MetaspaceShared::get_relocated_klass(ak);
        archived_m->metadata_field_put(_array_klass_offset, reloc_ak);
      }

      // Clear the fields. Just to be safe
      Klass *k = m->klass();
      Handle archived_mirror_h(THREAD, archived_m);
      ResetMirrorField reset(archived_mirror_h);
      InstanceKlass::cast(k)->do_nonstatic_fields(&reset);

      log_trace(cds, heap, mirror)(
        "Archived %s mirror object from " PTR_FORMAT " ==> " PTR_FORMAT,
        type2name((BasicType)t), p2i(Universe::_mirrors[t]), p2i(archived_m));

      Universe::_mirrors[t] = archived_m;
    }
  }

  assert(Universe::_mirrors[T_INT] != NULL &&
         Universe::_mirrors[T_FLOAT] != NULL &&
         Universe::_mirrors[T_DOUBLE] != NULL &&
         Universe::_mirrors[T_BYTE] != NULL &&
         Universe::_mirrors[T_BOOLEAN] != NULL &&
         Universe::_mirrors[T_CHAR] != NULL &&
         Universe::_mirrors[T_LONG] != NULL &&
         Universe::_mirrors[T_SHORT] != NULL &&
         Universe::_mirrors[T_VOID] != NULL, "sanity");

  Universe::set_int_mirror(Universe::_mirrors[T_INT]);
  Universe::set_float_mirror(Universe::_mirrors[T_FLOAT]);
  Universe::set_double_mirror(Universe::_mirrors[T_DOUBLE]);
  Universe::set_byte_mirror(Universe::_mirrors[T_BYTE]);
  Universe::set_bool_mirror(Universe::_mirrors[T_BOOLEAN]);
  Universe::set_char_mirror(Universe::_mirrors[T_CHAR]);
  Universe::set_long_mirror(Universe::_mirrors[T_LONG]);
  Universe::set_short_mirror(Universe::_mirrors[T_SHORT]);
  Universe::set_void_mirror(Universe::_mirrors[T_VOID]);
}

//
// After the mirror object is successfully archived, the archived
// klass is set with _has_archived_raw_mirror flag.
//
// The _has_archived_raw_mirror flag is cleared at runtime when the
// archived mirror is restored. If archived java heap data cannot
// be used at runtime, new mirror object is created for the shared
// class. The _has_archived_raw_mirror is cleared also during the process.
oop java_lang_Class::archive_mirror(Klass* k, TRAPS) {
  assert(HeapShared::is_heap_object_archiving_allowed(),
         "HeapShared::is_heap_object_archiving_allowed() must be true");

  // Mirror is already archived
  if (k->has_raw_archived_mirror()) {
    assert(k->archived_java_mirror_raw() != NULL, "no archived mirror");
    return k->archived_java_mirror_raw();
  }

  // No mirror
  oop mirror = k->java_mirror();
  if (mirror == NULL) {
    return NULL;
  }

  if (k->is_instance_klass()) {
    InstanceKlass *ik = InstanceKlass::cast(k);
    assert(ik->signers() == NULL, "class with signer should have been excluded");

    if (!(ik->is_shared_boot_class() || ik->is_shared_platform_class() ||
          ik->is_shared_app_class())) {
      // Archiving mirror for classes from non-builtin loaders is not
      // supported. Clear the _java_mirror within the archived class.
      k->set_java_mirror_handle(NULL);
      return NULL;
    }
  }

  // Now start archiving the mirror object
  oop archived_mirror = HeapShared::archive_heap_object(mirror, THREAD);
  if (archived_mirror == NULL) {
    return NULL;
  }

  archived_mirror = process_archived_mirror(k, mirror, archived_mirror, THREAD);
  if (archived_mirror == NULL) {
    return NULL;
  }

  k->set_archived_java_mirror_raw(archived_mirror);

  k->set_has_raw_archived_mirror();

  ResourceMark rm;
  log_trace(cds, heap, mirror)(
    "Archived %s mirror object from " PTR_FORMAT " ==> " PTR_FORMAT,
    k->external_name(), p2i(mirror), p2i(archived_mirror));

  return archived_mirror;
}

// The process is based on create_mirror().
oop java_lang_Class::process_archived_mirror(Klass* k, oop mirror,
                                             oop archived_mirror,
                                             Thread *THREAD) {
  // Clear nonstatic fields in archived mirror. Some of the fields will be set
  // to archived metadata and objects below.
  Klass *c = archived_mirror->klass();
  Handle archived_mirror_h(THREAD, archived_mirror);
  ResetMirrorField reset(archived_mirror_h);
  InstanceKlass::cast(c)->do_nonstatic_fields(&reset);

  if (k->is_array_klass()) {
    oop archived_comp_mirror;
    if (k->is_typeArray_klass()) {
      // The primitive type mirrors are already archived. Get the archived mirror.
      oop comp_mirror = java_lang_Class::component_mirror(mirror);
      archived_comp_mirror = HeapShared::find_archived_heap_object(comp_mirror);
      assert(archived_comp_mirror != NULL, "Must be");
    } else {
      assert(k->is_objArray_klass(), "Must be");
      Klass* element_klass = ObjArrayKlass::cast(k)->element_klass();
      assert(element_klass != NULL, "Must have an element klass");
      archived_comp_mirror = archive_mirror(element_klass, THREAD);
      if (archived_comp_mirror == NULL) {
        return NULL;
      }
    }
    java_lang_Class::set_component_mirror(archived_mirror, archived_comp_mirror);
  } else {
    assert(k->is_instance_klass(), "Must be");

    // Reset local static fields in the mirror
    InstanceKlass::cast(k)->do_local_static_fields(&reset);

    java_lang_Class:set_init_lock(archived_mirror, NULL);

    set_protection_domain(archived_mirror, NULL);
  }

  // clear class loader and mirror_module_field
  set_class_loader(archived_mirror, NULL);
  set_module(archived_mirror, NULL);

  // The archived mirror's field at _klass_offset is still pointing to the original
  // klass. Updated the field in the archived mirror to point to the relocated
  // klass in the archive.
  Klass *reloc_k = MetaspaceShared::get_relocated_klass(as_Klass(mirror));
  log_debug(cds, heap, mirror)(
    "Relocate mirror metadata field at _klass_offset from " PTR_FORMAT " ==> " PTR_FORMAT,
    p2i(as_Klass(mirror)), p2i(reloc_k));
  archived_mirror->metadata_field_put(_klass_offset, reloc_k);

  // The field at _array_klass_offset is pointing to the original one dimension
  // higher array klass if exists. Relocate the pointer.
  Klass *arr = array_klass_acquire(mirror);
  if (arr != NULL) {
    Klass *reloc_arr = MetaspaceShared::get_relocated_klass(arr);
    log_debug(cds, heap, mirror)(
      "Relocate mirror metadata field at _array_klass_offset from " PTR_FORMAT " ==> " PTR_FORMAT,
      p2i(arr), p2i(reloc_arr));
    archived_mirror->metadata_field_put(_array_klass_offset, reloc_arr);
  }
  return archived_mirror;
}

// Returns true if the mirror is updated, false if no archived mirror
// data is present. After the archived mirror object is restored, the
// shared klass' _has_raw_archived_mirror flag is cleared.
bool java_lang_Class::restore_archived_mirror(Klass *k,
                                              Handle class_loader, Handle module,
                                              Handle protection_domain, TRAPS) {
  // Postpone restoring archived mirror until java.lang.Class is loaded. Please
  // see more details in SystemDictionary::resolve_well_known_classes().
  if (!SystemDictionary::Class_klass_loaded()) {
    assert(fixup_mirror_list() != NULL, "fixup_mirror_list not initialized");
    fixup_mirror_list()->push(k);
    return true;
  }

  oop m = HeapShared::materialize_archived_object(k->archived_java_mirror_raw_narrow());

  if (m == NULL) {
    return false;
  }

  log_debug(cds, mirror)("Archived mirror is: " PTR_FORMAT, p2i(m));

  // mirror is archived, restore
  assert(HeapShared::is_archived_object(m), "must be archived mirror object");
  Handle mirror(THREAD, m);

  if (!k->is_array_klass()) {
    // - local static final fields with initial values were initialized at dump time

    // create the init_lock
    typeArrayOop r = oopFactory::new_typeArray(T_INT, 0, CHECK_(false));
    set_init_lock(mirror(), r);

    if (protection_domain.not_null()) {
      set_protection_domain(mirror(), protection_domain());
    }
  }

  assert(class_loader() == k->class_loader(), "should be same");
  if (class_loader.not_null()) {
    set_class_loader(mirror(), class_loader());
  }

  k->set_java_mirror(mirror);
  k->clear_has_raw_archived_mirror();

  set_mirror_module_field(k, mirror, module, THREAD);

  ResourceMark rm;
  log_trace(cds, heap, mirror)(
    "Restored %s archived mirror " PTR_FORMAT, k->external_name(), p2i(mirror()));

  return true;
}
#endif // INCLUDE_CDS_JAVA_HEAP

void java_lang_Class::fixup_module_field(Klass* k, Handle module) {
  assert(_module_offset != 0, "must have been computed already");
  java_lang_Class::set_module(k->java_mirror(), module());
}

int  java_lang_Class::oop_size(oop java_class) {
  assert(_oop_size_offset != 0, "must be set");
  int size = java_class->int_field(_oop_size_offset);
  assert(size > 0, "Oop size must be greater than zero, not %d", size);
  return size;
}


void java_lang_Class::set_oop_size(HeapWord* java_class, int size) {
  assert(_oop_size_offset != 0, "must be set");
  assert(size > 0, "Oop size must be greater than zero, not %d", size);
  *(int*)(((char*)java_class) + _oop_size_offset) = size;
}

int  java_lang_Class::static_oop_field_count(oop java_class) {
  assert(_static_oop_field_count_offset != 0, "must be set");
  return java_class->int_field(_static_oop_field_count_offset);
}

int  java_lang_Class::static_oop_field_count_raw(oop java_class) {
  assert(_static_oop_field_count_offset != 0, "must be set");
  return java_class->int_field_raw(_static_oop_field_count_offset);
}

void java_lang_Class::set_static_oop_field_count(oop java_class, int size) {
  assert(_static_oop_field_count_offset != 0, "must be set");
  java_class->int_field_put(_static_oop_field_count_offset, size);
}

oop java_lang_Class::protection_domain(oop java_class) {
  assert(_protection_domain_offset != 0, "must be set");
  return java_class->obj_field(_protection_domain_offset);
}
void java_lang_Class::set_protection_domain(oop java_class, oop pd) {
  assert(_protection_domain_offset != 0, "must be set");
  java_class->obj_field_put(_protection_domain_offset, pd);
}

void java_lang_Class::set_component_mirror(oop java_class, oop comp_mirror) {
  assert(_component_mirror_offset != 0, "must be set");
    java_class->obj_field_put(_component_mirror_offset, comp_mirror);
  }
oop java_lang_Class::component_mirror(oop java_class) {
  assert(_component_mirror_offset != 0, "must be set");
  return java_class->obj_field(_component_mirror_offset);
}

oop java_lang_Class::init_lock(oop java_class) {
  assert(_init_lock_offset != 0, "must be set");
  return java_class->obj_field(_init_lock_offset);
}
void java_lang_Class::set_init_lock(oop java_class, oop init_lock) {
  assert(_init_lock_offset != 0, "must be set");
  java_class->obj_field_put(_init_lock_offset, init_lock);
}

objArrayOop java_lang_Class::signers(oop java_class) {
  assert(_signers_offset != 0, "must be set");
  return (objArrayOop)java_class->obj_field(_signers_offset);
}
void java_lang_Class::set_signers(oop java_class, objArrayOop signers) {
  assert(_signers_offset != 0, "must be set");
  java_class->obj_field_put(_signers_offset, (oop)signers);
}


void java_lang_Class::set_class_loader(oop java_class, oop loader) {
  // jdk7 runs Queens in bootstrapping and jdk8-9 has no coordinated pushes yet.
  if (_class_loader_offset != 0) {
    java_class->obj_field_put(_class_loader_offset, loader);
  }
}

oop java_lang_Class::class_loader(oop java_class) {
  assert(_class_loader_offset != 0, "must be set");
  return java_class->obj_field(_class_loader_offset);
}

oop java_lang_Class::module(oop java_class) {
  assert(_module_offset != 0, "must be set");
  return java_class->obj_field(_module_offset);
}

void java_lang_Class::set_module(oop java_class, oop module) {
  assert(_module_offset != 0, "must be set");
  java_class->obj_field_put(_module_offset, module);
}

oop java_lang_Class::name(Handle java_class, TRAPS) {
  assert(_name_offset != 0, "must be set");
  oop o = java_class->obj_field(_name_offset);
  if (o == NULL) {
    o = StringTable::intern(java_lang_Class::as_external_name(java_class()), THREAD);
    java_class->obj_field_put(_name_offset, o);
  }
  return o;
}

oop java_lang_Class::source_file(oop java_class) {
  assert(_source_file_offset != 0, "must be set");
  return java_class->obj_field(_source_file_offset);
}

void java_lang_Class::set_source_file(oop java_class, oop source_file) {
  assert(_source_file_offset != 0, "must be set");
  java_class->obj_field_put(_source_file_offset, source_file);
}

oop java_lang_Class::create_basic_type_mirror(const char* basic_type_name, BasicType type, TRAPS) {
  // This should be improved by adding a field at the Java level or by
  // introducing a new VM klass (see comment in ClassFileParser)
  oop java_class = InstanceMirrorKlass::cast(SystemDictionary::Class_klass())->allocate_instance(NULL, CHECK_0);
  if (type != T_VOID) {
    Klass* aklass = Universe::typeArrayKlassObj(type);
    assert(aklass != NULL, "correct bootstrap");
    release_set_array_klass(java_class, aklass);
  }
#ifdef ASSERT
  InstanceMirrorKlass* mk = InstanceMirrorKlass::cast(SystemDictionary::Class_klass());
  assert(java_lang_Class::static_oop_field_count(java_class) == 0, "should have been zeroed by allocation");
#endif
  return java_class;
}


Klass* java_lang_Class::as_Klass(oop java_class) {
  //%note memory_2
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  Klass* k = ((Klass*)java_class->metadata_field(_klass_offset));
  assert(k == NULL || k->is_klass(), "type check");
  return k;
}

Klass* java_lang_Class::as_Klass_raw(oop java_class) {
  //%note memory_2
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  Klass* k = ((Klass*)java_class->metadata_field_raw(_klass_offset));
  assert(k == NULL || k->is_klass(), "type check");
  return k;
}


void java_lang_Class::set_klass(oop java_class, Klass* klass) {
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  java_class->metadata_field_put(_klass_offset, klass);
}


void java_lang_Class::print_signature(oop java_class, outputStream* st) {
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  Symbol* name = NULL;
  bool is_instance = false;
  if (is_primitive(java_class)) {
    name = vmSymbols::type_signature(primitive_type(java_class));
  } else {
    Klass* k = as_Klass(java_class);
    is_instance = k->is_instance_klass();
    name = k->name();
  }
  if (name == NULL) {
    st->print("<null>");
    return;
  }
  if (is_instance)  st->print("L");
  st->write((char*) name->base(), (int) name->utf8_length());
  if (is_instance)  st->print(";");
}

Symbol* java_lang_Class::as_signature(oop java_class, bool intern_if_not_found) {
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  Symbol* name;
  if (is_primitive(java_class)) {
    name = vmSymbols::type_signature(primitive_type(java_class));
    // Because this can create a new symbol, the caller has to decrement
    // the refcount, so make adjustment here and below for symbols returned
    // that are not created or incremented due to a successful lookup.
    name->increment_refcount();
  } else {
    Klass* k = as_Klass(java_class);
    if (!k->is_instance_klass()) {
      name = k->name();
      name->increment_refcount();
    } else {
      ResourceMark rm;
      const char* sigstr = k->signature_name();
      int         siglen = (int) strlen(sigstr);
      if (!intern_if_not_found) {
        name = SymbolTable::probe(sigstr, siglen);
      } else {
        name = SymbolTable::new_symbol(sigstr, siglen);
      }
    }
  }
  return name;
}

// Returns the Java name for this Java mirror (Resource allocated)
// See Klass::external_name().
// For primitive type Java mirrors, its type name is returned.
const char* java_lang_Class::as_external_name(oop java_class) {
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  const char* name = NULL;
  if (is_primitive(java_class)) {
    name = type2name(primitive_type(java_class));
  } else {
    name = as_Klass(java_class)->external_name();
  }
  if (name == NULL) {
    name = "<null>";
  }
  return name;
}

Klass* java_lang_Class::array_klass_acquire(oop java_class) {
  Klass* k = ((Klass*)java_class->metadata_field_acquire(_array_klass_offset));
  assert(k == NULL || k->is_klass() && k->is_array_klass(), "should be array klass");
  return k;
}


void java_lang_Class::release_set_array_klass(oop java_class, Klass* klass) {
  assert(klass->is_klass() && klass->is_array_klass(), "should be array klass");
  java_class->release_metadata_field_put(_array_klass_offset, klass);
}


BasicType java_lang_Class::primitive_type(oop java_class) {
  assert(java_lang_Class::is_primitive(java_class), "just checking");
  Klass* ak = ((Klass*)java_class->metadata_field(_array_klass_offset));
  BasicType type = T_VOID;
  if (ak != NULL) {
    // Note: create_basic_type_mirror above initializes ak to a non-null value.
    type = ArrayKlass::cast(ak)->element_type();
  } else {
    assert(java_class == Universe::void_mirror(), "only valid non-array primitive");
  }
  assert(Universe::java_mirror(type) == java_class, "must be consistent");
  return type;
}

BasicType java_lang_Class::as_BasicType(oop java_class, Klass** reference_klass) {
  assert(java_lang_Class::is_instance(java_class), "must be a Class object");
  if (is_primitive(java_class)) {
    if (reference_klass != NULL)
      (*reference_klass) = NULL;
    return primitive_type(java_class);
  } else {
    if (reference_klass != NULL)
      (*reference_klass) = as_Klass(java_class);
    return T_OBJECT;
  }
}


oop java_lang_Class::primitive_mirror(BasicType t) {
  oop mirror = Universe::java_mirror(t);
  assert(mirror != NULL && mirror->is_a(SystemDictionary::Class_klass()), "must be a Class");
  assert(java_lang_Class::is_primitive(mirror), "must be primitive");
  return mirror;
}

bool java_lang_Class::offsets_computed = false;
int  java_lang_Class::classRedefinedCount_offset = -1;

#define CLASS_FIELDS_DO(macro) \
  macro(classRedefinedCount_offset, k, "classRedefinedCount", int_signature,         false); \
  macro(_class_loader_offset,       k, "classLoader",         classloader_signature, false); \
  macro(_component_mirror_offset,   k, "componentType",       class_signature,       false); \
  macro(_module_offset,             k, "module",              module_signature,      false); \
  macro(_name_offset,               k, "name",                string_signature,      false); \

void java_lang_Class::compute_offsets() {
  if (offsets_computed) {
    return;
  }

  offsets_computed = true;

  InstanceKlass* k = SystemDictionary::Class_klass();
  CLASS_FIELDS_DO(FIELD_COMPUTE_OFFSET);

  // Init lock is a C union with component_mirror.  Only instanceKlass mirrors have
  // init_lock and only ArrayKlass mirrors have component_mirror.  Since both are oops
  // GC treats them the same.
  _init_lock_offset = _component_mirror_offset;

  CLASS_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_Class::serialize_offsets(SerializeClosure* f) {
  f->do_bool(&offsets_computed);
  f->do_u4((u4*)&_init_lock_offset);

  CLASS_FIELDS_DO(FIELD_SERIALIZE_OFFSET);

  CLASS_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

int java_lang_Class::classRedefinedCount(oop the_class_mirror) {
  if (classRedefinedCount_offset == -1) {
    // If we don't have an offset for it then just return -1 as a marker.
    return -1;
  }

  return the_class_mirror->int_field(classRedefinedCount_offset);
}

void java_lang_Class::set_classRedefinedCount(oop the_class_mirror, int value) {
  if (classRedefinedCount_offset == -1) {
    // If we don't have an offset for it then nothing to set.
    return;
  }

  the_class_mirror->int_field_put(classRedefinedCount_offset, value);
}


// Note: JDK1.1 and before had a privateInfo_offset field which was used for the
//       platform thread structure, and a eetop offset which was used for thread
//       local storage (and unused by the HotSpot VM). In JDK1.2 the two structures
//       merged, so in the HotSpot VM we just use the eetop field for the thread
//       instead of the privateInfo_offset.
//
// Note: The stackSize field is only present starting in 1.4.

int java_lang_Thread::_name_offset = 0;
int java_lang_Thread::_group_offset = 0;
int java_lang_Thread::_contextClassLoader_offset = 0;
int java_lang_Thread::_inheritedAccessControlContext_offset = 0;
int java_lang_Thread::_priority_offset = 0;
int java_lang_Thread::_eetop_offset = 0;
int java_lang_Thread::_daemon_offset = 0;
int java_lang_Thread::_stillborn_offset = 0;
int java_lang_Thread::_stackSize_offset = 0;
int java_lang_Thread::_tid_offset = 0;
int java_lang_Thread::_thread_status_offset = 0;
int java_lang_Thread::_park_blocker_offset = 0;

#define THREAD_FIELDS_DO(macro) \
  macro(_name_offset,          k, vmSymbols::name_name(), string_signature, false); \
  macro(_group_offset,         k, vmSymbols::group_name(), threadgroup_signature, false); \
  macro(_contextClassLoader_offset, k, vmSymbols::contextClassLoader_name(), classloader_signature, false); \
  macro(_inheritedAccessControlContext_offset, k, vmSymbols::inheritedAccessControlContext_name(), accesscontrolcontext_signature, false); \
  macro(_priority_offset,      k, vmSymbols::priority_name(), int_signature, false); \
  macro(_daemon_offset,        k, vmSymbols::daemon_name(), bool_signature, false); \
  macro(_eetop_offset,         k, "eetop", long_signature, false); \
  macro(_stillborn_offset,     k, "stillborn", bool_signature, false); \
  macro(_stackSize_offset,     k, "stackSize", long_signature, false); \
  macro(_tid_offset,           k, "tid", long_signature, false); \
  macro(_thread_status_offset, k, "threadStatus", int_signature, false); \
  macro(_park_blocker_offset,  k, "parkBlocker", object_signature, false)

void java_lang_Thread::compute_offsets() {
  assert(_group_offset == 0, "offsets should be initialized only once");

  InstanceKlass* k = SystemDictionary::Thread_klass();
  THREAD_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_Thread::serialize_offsets(SerializeClosure* f) {
  THREAD_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

JavaThread* java_lang_Thread::thread(oop java_thread) {
  return (JavaThread*)java_thread->address_field(_eetop_offset);
}


void java_lang_Thread::set_thread(oop java_thread, JavaThread* thread) {
  java_thread->address_field_put(_eetop_offset, (address)thread);
}


oop java_lang_Thread::name(oop java_thread) {
  return java_thread->obj_field(_name_offset);
}


void java_lang_Thread::set_name(oop java_thread, oop name) {
  java_thread->obj_field_put(_name_offset, name);
}


ThreadPriority java_lang_Thread::priority(oop java_thread) {
  return (ThreadPriority)java_thread->int_field(_priority_offset);
}


void java_lang_Thread::set_priority(oop java_thread, ThreadPriority priority) {
  java_thread->int_field_put(_priority_offset, priority);
}


oop java_lang_Thread::threadGroup(oop java_thread) {
  return java_thread->obj_field(_group_offset);
}


bool java_lang_Thread::is_stillborn(oop java_thread) {
  return java_thread->bool_field(_stillborn_offset) != 0;
}


// We never have reason to turn the stillborn bit off
void java_lang_Thread::set_stillborn(oop java_thread) {
  java_thread->bool_field_put(_stillborn_offset, true);
}


bool java_lang_Thread::is_alive(oop java_thread) {
  JavaThread* thr = java_lang_Thread::thread(java_thread);
  return (thr != NULL);
}


bool java_lang_Thread::is_daemon(oop java_thread) {
  return java_thread->bool_field(_daemon_offset) != 0;
}


void java_lang_Thread::set_daemon(oop java_thread) {
  java_thread->bool_field_put(_daemon_offset, true);
}

oop java_lang_Thread::context_class_loader(oop java_thread) {
  return java_thread->obj_field(_contextClassLoader_offset);
}

oop java_lang_Thread::inherited_access_control_context(oop java_thread) {
  return java_thread->obj_field(_inheritedAccessControlContext_offset);
}


jlong java_lang_Thread::stackSize(oop java_thread) {
  return java_thread->long_field(_stackSize_offset);
}

// Write the thread status value to threadStatus field in java.lang.Thread java class.
void java_lang_Thread::set_thread_status(oop java_thread,
                                         java_lang_Thread::ThreadStatus status) {
  java_thread->int_field_put(_thread_status_offset, status);
}

// Read thread status value from threadStatus field in java.lang.Thread java class.
java_lang_Thread::ThreadStatus java_lang_Thread::get_thread_status(oop java_thread) {
  // Make sure the caller is operating on behalf of the VM or is
  // running VM code (state == _thread_in_vm).
  assert(Threads_lock->owned_by_self() || Thread::current()->is_VM_thread() ||
         JavaThread::current()->thread_state() == _thread_in_vm,
         "Java Thread is not running in vm");
  return (java_lang_Thread::ThreadStatus)java_thread->int_field(_thread_status_offset);
}


jlong java_lang_Thread::thread_id(oop java_thread) {
  return java_thread->long_field(_tid_offset);
}

oop java_lang_Thread::park_blocker(oop java_thread) {
  return java_thread->obj_field(_park_blocker_offset);
}

const char* java_lang_Thread::thread_status_name(oop java_thread) {
  ThreadStatus status = (java_lang_Thread::ThreadStatus)java_thread->int_field(_thread_status_offset);
  switch (status) {
    case NEW                      : return "NEW";
    case RUNNABLE                 : return "RUNNABLE";
    case SLEEPING                 : return "TIMED_WAITING (sleeping)";
    case IN_OBJECT_WAIT           : return "WAITING (on object monitor)";
    case IN_OBJECT_WAIT_TIMED     : return "TIMED_WAITING (on object monitor)";
    case PARKED                   : return "WAITING (parking)";
    case PARKED_TIMED             : return "TIMED_WAITING (parking)";
    case BLOCKED_ON_MONITOR_ENTER : return "BLOCKED (on object monitor)";
    case TERMINATED               : return "TERMINATED";
    default                       : return "UNKNOWN";
  };
}
int java_lang_ThreadGroup::_parent_offset = 0;
int java_lang_ThreadGroup::_name_offset = 0;
int java_lang_ThreadGroup::_threads_offset = 0;
int java_lang_ThreadGroup::_groups_offset = 0;
int java_lang_ThreadGroup::_maxPriority_offset = 0;
int java_lang_ThreadGroup::_destroyed_offset = 0;
int java_lang_ThreadGroup::_daemon_offset = 0;
int java_lang_ThreadGroup::_nthreads_offset = 0;
int java_lang_ThreadGroup::_ngroups_offset = 0;

oop  java_lang_ThreadGroup::parent(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return java_thread_group->obj_field(_parent_offset);
}

// ("name as oop" accessor is not necessary)

const char* java_lang_ThreadGroup::name(oop java_thread_group) {
  oop name = java_thread_group->obj_field(_name_offset);
  // ThreadGroup.name can be null
  if (name != NULL) {
    return java_lang_String::as_utf8_string(name);
  }
  return NULL;
}

int java_lang_ThreadGroup::nthreads(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return java_thread_group->int_field(_nthreads_offset);
}

objArrayOop java_lang_ThreadGroup::threads(oop java_thread_group) {
  oop threads = java_thread_group->obj_field(_threads_offset);
  assert(threads != NULL, "threadgroups should have threads");
  assert(threads->is_objArray(), "just checking"); // Todo: Add better type checking code
  return objArrayOop(threads);
}

int java_lang_ThreadGroup::ngroups(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return java_thread_group->int_field(_ngroups_offset);
}

objArrayOop java_lang_ThreadGroup::groups(oop java_thread_group) {
  oop groups = java_thread_group->obj_field(_groups_offset);
  assert(groups == NULL || groups->is_objArray(), "just checking"); // Todo: Add better type checking code
  return objArrayOop(groups);
}

ThreadPriority java_lang_ThreadGroup::maxPriority(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return (ThreadPriority) java_thread_group->int_field(_maxPriority_offset);
}

bool java_lang_ThreadGroup::is_destroyed(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return java_thread_group->bool_field(_destroyed_offset) != 0;
}

bool java_lang_ThreadGroup::is_daemon(oop java_thread_group) {
  assert(oopDesc::is_oop(java_thread_group), "thread group must be oop");
  return java_thread_group->bool_field(_daemon_offset) != 0;
}

#define THREADGROUP_FIELDS_DO(macro) \
  macro(_parent_offset,      k, vmSymbols::parent_name(),      threadgroup_signature,       false); \
  macro(_name_offset,        k, vmSymbols::name_name(),        string_signature,            false); \
  macro(_threads_offset,     k, vmSymbols::threads_name(),     thread_array_signature,      false); \
  macro(_groups_offset,      k, vmSymbols::groups_name(),      threadgroup_array_signature, false); \
  macro(_maxPriority_offset, k, vmSymbols::maxPriority_name(), int_signature,               false); \
  macro(_destroyed_offset,   k, vmSymbols::destroyed_name(),   bool_signature,              false); \
  macro(_daemon_offset,      k, vmSymbols::daemon_name(),      bool_signature,              false); \
  macro(_nthreads_offset,    k, vmSymbols::nthreads_name(),    int_signature,               false); \
  macro(_ngroups_offset,     k, vmSymbols::ngroups_name(),     int_signature,               false)

void java_lang_ThreadGroup::compute_offsets() {
  assert(_parent_offset == 0, "offsets should be initialized only once");

  InstanceKlass* k = SystemDictionary::ThreadGroup_klass();
  THREADGROUP_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_ThreadGroup::serialize_offsets(SerializeClosure* f) {
  THREADGROUP_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define THROWABLE_FIELDS_DO(macro) \
  macro(backtrace_offset,     k, "backtrace",     object_signature,                  false); \
  macro(detailMessage_offset, k, "detailMessage", string_signature,                  false); \
  macro(stackTrace_offset,    k, "stackTrace",    java_lang_StackTraceElement_array, false); \
  macro(depth_offset,         k, "depth",         int_signature,                     false); \
  macro(static_unassigned_stacktrace_offset, k, "UNASSIGNED_STACK", java_lang_StackTraceElement_array, true)

void java_lang_Throwable::compute_offsets() {
  InstanceKlass* k = SystemDictionary::Throwable_klass();
  THROWABLE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_Throwable::serialize_offsets(SerializeClosure* f) {
  THROWABLE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_lang_Throwable::unassigned_stacktrace() {
  InstanceKlass* ik = SystemDictionary::Throwable_klass();
  oop base = ik->static_field_base_raw();
  return base->obj_field(static_unassigned_stacktrace_offset);
}

oop java_lang_Throwable::backtrace(oop throwable) {
  return throwable->obj_field_acquire(backtrace_offset);
}


void java_lang_Throwable::set_backtrace(oop throwable, oop value) {
  throwable->release_obj_field_put(backtrace_offset, value);
}

int java_lang_Throwable::depth(oop throwable) {
  return throwable->int_field(depth_offset);
}

void java_lang_Throwable::set_depth(oop throwable, int value) {
  throwable->int_field_put(depth_offset, value);
}

oop java_lang_Throwable::message(oop throwable) {
  return throwable->obj_field(detailMessage_offset);
}


// Return Symbol for detailed_message or NULL
Symbol* java_lang_Throwable::detail_message(oop throwable) {
  PRESERVE_EXCEPTION_MARK;  // Keep original exception
  oop detailed_message = java_lang_Throwable::message(throwable);
  if (detailed_message != NULL) {
    return java_lang_String::as_symbol(detailed_message);
  }
  return NULL;
}

void java_lang_Throwable::set_message(oop throwable, oop value) {
  throwable->obj_field_put(detailMessage_offset, value);
}


void java_lang_Throwable::set_stacktrace(oop throwable, oop st_element_array) {
  throwable->obj_field_put(stackTrace_offset, st_element_array);
}

void java_lang_Throwable::clear_stacktrace(oop throwable) {
  set_stacktrace(throwable, NULL);
}


void java_lang_Throwable::print(oop throwable, outputStream* st) {
  ResourceMark rm;
  Klass* k = throwable->klass();
  assert(k != NULL, "just checking");
  st->print("%s", k->external_name());
  oop msg = message(throwable);
  if (msg != NULL) {
    st->print(": %s", java_lang_String::as_utf8_string(msg));
  }
}

// After this many redefines, the stack trace is unreliable.
const int MAX_VERSION = USHRT_MAX;

static inline bool version_matches(Method* method, int version) {
  assert(version < MAX_VERSION, "version is too big");
  return method != NULL && (method->constants()->version() == version);
}

// This class provides a simple wrapper over the internal structure of
// exception backtrace to insulate users of the backtrace from needing
// to know what it looks like.
class BacktraceBuilder: public StackObj {
 friend class BacktraceIterator;
 private:
  Handle          _backtrace;
  objArrayOop     _head;
  typeArrayOop    _methods;
  typeArrayOop    _bcis;
  objArrayOop     _mirrors;
  typeArrayOop    _names; // Needed to insulate method name against redefinition.
  // This is set to a java.lang.Boolean(true) if the top frame
  // of the backtrace is omitted because it shall be hidden.
  // Else it is null.
  oop             _has_hidden_top_frame;
  int             _index;
  NoSafepointVerifier _nsv;

  enum {
    trace_methods_offset = java_lang_Throwable::trace_methods_offset,
    trace_bcis_offset    = java_lang_Throwable::trace_bcis_offset,
    trace_mirrors_offset = java_lang_Throwable::trace_mirrors_offset,
    trace_names_offset   = java_lang_Throwable::trace_names_offset,
    trace_next_offset    = java_lang_Throwable::trace_next_offset,
    trace_hidden_offset  = java_lang_Throwable::trace_hidden_offset,
    trace_size           = java_lang_Throwable::trace_size,
    trace_chunk_size     = java_lang_Throwable::trace_chunk_size
  };

  // get info out of chunks
  static typeArrayOop get_methods(objArrayHandle chunk) {
    typeArrayOop methods = typeArrayOop(chunk->obj_at(trace_methods_offset));
    assert(methods != NULL, "method array should be initialized in backtrace");
    return methods;
  }
  static typeArrayOop get_bcis(objArrayHandle chunk) {
    typeArrayOop bcis = typeArrayOop(chunk->obj_at(trace_bcis_offset));
    assert(bcis != NULL, "bci array should be initialized in backtrace");
    return bcis;
  }
  static objArrayOop get_mirrors(objArrayHandle chunk) {
    objArrayOop mirrors = objArrayOop(chunk->obj_at(trace_mirrors_offset));
    assert(mirrors != NULL, "mirror array should be initialized in backtrace");
    return mirrors;
  }
  static typeArrayOop get_names(objArrayHandle chunk) {
    typeArrayOop names = typeArrayOop(chunk->obj_at(trace_names_offset));
    assert(names != NULL, "names array should be initialized in backtrace");
    return names;
  }
  static oop get_has_hidden_top_frame(objArrayHandle chunk) {
    oop hidden = chunk->obj_at(trace_hidden_offset);
    return hidden;
  }

 public:

  // constructor for new backtrace
  BacktraceBuilder(TRAPS): _head(NULL), _methods(NULL), _bcis(NULL), _mirrors(NULL), _names(NULL), _has_hidden_top_frame(NULL) {
    expand(CHECK);
    _backtrace = Handle(THREAD, _head);
    _index = 0;
  }

  BacktraceBuilder(Thread* thread, objArrayHandle backtrace) {
    _methods = get_methods(backtrace);
    _bcis = get_bcis(backtrace);
    _mirrors = get_mirrors(backtrace);
    _names = get_names(backtrace);
    _has_hidden_top_frame = get_has_hidden_top_frame(backtrace);
    assert(_methods->length() == _bcis->length() &&
           _methods->length() == _mirrors->length() &&
           _mirrors->length() == _names->length(),
           "method and source information arrays should match");

    // head is the preallocated backtrace
    _head = backtrace();
    _backtrace = Handle(thread, _head);
    _index = 0;
  }

  void expand(TRAPS) {
    objArrayHandle old_head(THREAD, _head);
    PauseNoSafepointVerifier pnsv(&_nsv);

    objArrayOop head = oopFactory::new_objectArray(trace_size, CHECK);
    objArrayHandle new_head(THREAD, head);

    typeArrayOop methods = oopFactory::new_shortArray(trace_chunk_size, CHECK);
    typeArrayHandle new_methods(THREAD, methods);

    typeArrayOop bcis = oopFactory::new_intArray(trace_chunk_size, CHECK);
    typeArrayHandle new_bcis(THREAD, bcis);

    objArrayOop mirrors = oopFactory::new_objectArray(trace_chunk_size, CHECK);
    objArrayHandle new_mirrors(THREAD, mirrors);

    typeArrayOop names = oopFactory::new_symbolArray(trace_chunk_size, CHECK);
    typeArrayHandle new_names(THREAD, names);

    if (!old_head.is_null()) {
      old_head->obj_at_put(trace_next_offset, new_head());
    }
    new_head->obj_at_put(trace_methods_offset, new_methods());
    new_head->obj_at_put(trace_bcis_offset, new_bcis());
    new_head->obj_at_put(trace_mirrors_offset, new_mirrors());
    new_head->obj_at_put(trace_names_offset, new_names());
    new_head->obj_at_put(trace_hidden_offset, NULL);

    _head    = new_head();
    _methods = new_methods();
    _bcis = new_bcis();
    _mirrors = new_mirrors();
    _names  = new_names();
    _index = 0;
  }

  oop backtrace() {
    return _backtrace();
  }

  inline void push(Method* method, int bci, TRAPS) {
    // Smear the -1 bci to 0 since the array only holds unsigned
    // shorts.  The later line number lookup would just smear the -1
    // to a 0 even if it could be recorded.
    if (bci == SynchronizationEntryBCI) bci = 0;

    if (_index >= trace_chunk_size) {
      methodHandle mhandle(THREAD, method);
      expand(CHECK);
      method = mhandle();
    }

    _methods->ushort_at_put(_index, method->orig_method_idnum());
    _bcis->int_at_put(_index, Backtrace::merge_bci_and_version(bci, method->constants()->version()));

    // Note:this doesn't leak symbols because the mirror in the backtrace keeps the
    // klass owning the symbols alive so their refcounts aren't decremented.
    Symbol* name = method->name();
    _names->symbol_at_put(_index, name);

    // We need to save the mirrors in the backtrace to keep the class
    // from being unloaded while we still have this stack trace.
    assert(method->method_holder()->java_mirror() != NULL, "never push null for mirror");
    _mirrors->obj_at_put(_index, method->method_holder()->java_mirror());
    _index++;
  }

  void set_has_hidden_top_frame(TRAPS) {
    if (_has_hidden_top_frame == NULL) {
      jvalue prim;
      prim.z = 1;
      PauseNoSafepointVerifier pnsv(&_nsv);
      _has_hidden_top_frame = java_lang_boxing_object::create(T_BOOLEAN, &prim, CHECK);
      _head->obj_at_put(trace_hidden_offset, _has_hidden_top_frame);
    }
  }

};

struct BacktraceElement : public StackObj {
  int _method_id;
  int _bci;
  int _version;
  Symbol* _name;
  Handle _mirror;
  BacktraceElement(Handle mirror, int mid, int version, int bci, Symbol* name) :
                   _method_id(mid), _bci(bci), _version(version), _name(name), _mirror(mirror) {}
};

class BacktraceIterator : public StackObj {
  int _index;
  objArrayHandle  _result;
  objArrayHandle  _mirrors;
  typeArrayHandle _methods;
  typeArrayHandle _bcis;
  typeArrayHandle _names;

  void init(objArrayHandle result, Thread* thread) {
    // Get method id, bci, version and mirror from chunk
    _result = result;
    if (_result.not_null()) {
      _methods = typeArrayHandle(thread, BacktraceBuilder::get_methods(_result));
      _bcis = typeArrayHandle(thread, BacktraceBuilder::get_bcis(_result));
      _mirrors = objArrayHandle(thread, BacktraceBuilder::get_mirrors(_result));
      _names = typeArrayHandle(thread, BacktraceBuilder::get_names(_result));
      _index = 0;
    }
  }
 public:
  BacktraceIterator(objArrayHandle result, Thread* thread) {
    init(result, thread);
    assert(_methods.is_null() || _methods->length() == java_lang_Throwable::trace_chunk_size, "lengths don't match");
  }

  BacktraceElement next(Thread* thread) {
    BacktraceElement e (Handle(thread, _mirrors->obj_at(_index)),
                        _methods->ushort_at(_index),
                        Backtrace::version_at(_bcis->int_at(_index)),
                        Backtrace::bci_at(_bcis->int_at(_index)),
                        _names->symbol_at(_index));
    _index++;

    if (_index >= java_lang_Throwable::trace_chunk_size) {
      int next_offset = java_lang_Throwable::trace_next_offset;
      // Get next chunk
      objArrayHandle result (thread, objArrayOop(_result->obj_at(next_offset)));
      init(result, thread);
    }
    return e;
  }

  bool repeat() {
    return _result.not_null() && _mirrors->obj_at(_index) != NULL;
  }
};


// Print stack trace element to resource allocated buffer
static void print_stack_element_to_stream(outputStream* st, Handle mirror, int method_id,
                                          int version, int bci, Symbol* name) {
  ResourceMark rm;

  // Get strings and string lengths
  InstanceKlass* holder = InstanceKlass::cast(java_lang_Class::as_Klass(mirror()));
  const char* klass_name  = holder->external_name();
  int buf_len = (int)strlen(klass_name);

  char* method_name = name->as_C_string();
  buf_len += (int)strlen(method_name);

  char* source_file_name = NULL;
  Symbol* source = Backtrace::get_source_file_name(holder, version);
  if (source != NULL) {
    source_file_name = source->as_C_string();
    buf_len += (int)strlen(source_file_name);
  }

  char *module_name = NULL, *module_version = NULL;
  ModuleEntry* module = holder->module();
  if (module->is_named()) {
    module_name = module->name()->as_C_string();
    buf_len += (int)strlen(module_name);
    if (module->version() != NULL) {
      module_version = module->version()->as_C_string();
      buf_len += (int)strlen(module_version);
    }
  }

  // Allocate temporary buffer with extra space for formatting and line number
  char* buf = NEW_RESOURCE_ARRAY(char, buf_len + 64);

  // Print stack trace line in buffer
  sprintf(buf, "\tat %s.%s(", klass_name, method_name);

  // Print module information
  if (module_name != NULL) {
    if (module_version != NULL) {
      sprintf(buf + (int)strlen(buf), "%s@%s/", module_name, module_version);
    } else {
      sprintf(buf + (int)strlen(buf), "%s/", module_name);
    }
  }

  // The method can be NULL if the requested class version is gone
  Method* method = holder->method_with_orig_idnum(method_id, version);
  if (!version_matches(method, version)) {
    strcat(buf, "Redefined)");
  } else {
    int line_number = Backtrace::get_line_number(method, bci);
    if (line_number == -2) {
      strcat(buf, "Native Method)");
    } else {
      if (source_file_name != NULL && (line_number != -1)) {
        // Sourcename and linenumber
        sprintf(buf + (int)strlen(buf), "%s:%d)", source_file_name, line_number);
      } else if (source_file_name != NULL) {
        // Just sourcename
        sprintf(buf + (int)strlen(buf), "%s)", source_file_name);
      } else {
        // Neither sourcename nor linenumber
        sprintf(buf + (int)strlen(buf), "Unknown Source)");
      }
      CompiledMethod* nm = method->code();
      if (WizardMode && nm != NULL) {
        sprintf(buf + (int)strlen(buf), "(nmethod " INTPTR_FORMAT ")", (intptr_t)nm);
      }
    }
  }

  st->print_cr("%s", buf);
}

void java_lang_Throwable::print_stack_element(outputStream *st, const methodHandle& method, int bci) {
  Handle mirror (Thread::current(),  method->method_holder()->java_mirror());
  int method_id = method->orig_method_idnum();
  int version = method->constants()->version();
  print_stack_element_to_stream(st, mirror, method_id, version, bci, method->name());
}

/**
 * Print the throwable message and its stack trace plus all causes by walking the
 * cause chain.  The output looks the same as of Throwable.printStackTrace().
 */
void java_lang_Throwable::print_stack_trace(Handle throwable, outputStream* st) {
  // First, print the message.
  print(throwable(), st);
  st->cr();

  // Now print the stack trace.
  Thread* THREAD = Thread::current();
  while (throwable.not_null()) {
    objArrayHandle result (THREAD, objArrayOop(backtrace(throwable())));
    if (result.is_null()) {
      st->print_raw_cr("\t<<no stack trace available>>");
      return;
    }
    BacktraceIterator iter(result, THREAD);

    while (iter.repeat()) {
      BacktraceElement bte = iter.next(THREAD);
      print_stack_element_to_stream(st, bte._mirror, bte._method_id, bte._version, bte._bci, bte._name);
    }
    {
      // Call getCause() which doesn't necessarily return the _cause field.
      EXCEPTION_MARK;
      JavaValue cause(T_OBJECT);
      JavaCalls::call_virtual(&cause,
                              throwable,
                              throwable->klass(),
                              vmSymbols::getCause_name(),
                              vmSymbols::void_throwable_signature(),
                              THREAD);
      // Ignore any exceptions. we are in the middle of exception handling. Same as classic VM.
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION;
        throwable = Handle();
      } else {
        throwable = Handle(THREAD, (oop) cause.get_jobject());
        if (throwable.not_null()) {
          st->print("Caused by: ");
          print(throwable(), st);
          st->cr();
        }
      }
    }
  }
}

/**
 * Print the throwable stack trace by calling the Java method java.lang.Throwable.printStackTrace().
 */
void java_lang_Throwable::java_printStackTrace(Handle throwable, TRAPS) {
  assert(throwable->is_a(SystemDictionary::Throwable_klass()), "Throwable instance expected");
  JavaValue result(T_VOID);
  JavaCalls::call_virtual(&result,
                          throwable,
                          SystemDictionary::Throwable_klass(),
                          vmSymbols::printStackTrace_name(),
                          vmSymbols::void_method_signature(),
                          THREAD);
}

void java_lang_Throwable::fill_in_stack_trace(Handle throwable, const methodHandle& method, TRAPS) {
  if (!StackTraceInThrowable) return;
  ResourceMark rm(THREAD);

  // Start out by clearing the backtrace for this object, in case the VM
  // runs out of memory while allocating the stack trace
  set_backtrace(throwable(), NULL);
  // Clear lazily constructed Java level stacktrace if refilling occurs
  // This is unnecessary in 1.7+ but harmless
  clear_stacktrace(throwable());

  int max_depth = MaxJavaStackTraceDepth;
  JavaThread* thread = (JavaThread*)THREAD;

  BacktraceBuilder bt(CHECK);

  // If there is no Java frame just return the method that was being called
  // with bci 0
  if (!thread->has_last_Java_frame()) {
    if (max_depth >= 1 && method() != NULL) {
      bt.push(method(), 0, CHECK);
      log_info(stacktrace)("%s, %d", throwable->klass()->external_name(), 1);
      set_depth(throwable(), 1);
      set_backtrace(throwable(), bt.backtrace());
    }
    return;
  }

  // Instead of using vframe directly, this version of fill_in_stack_trace
  // basically handles everything by hand. This significantly improved the
  // speed of this method call up to 28.5% on Solaris sparc. 27.1% on Windows.
  // See bug 6333838 for  more details.
  // The "ASSERT" here is to verify this method generates the exactly same stack
  // trace as utilizing vframe.
#ifdef ASSERT
  vframeStream st(thread);
  methodHandle st_method(THREAD, st.method());
#endif
  int total_count = 0;
  RegisterMap map(thread, false);
  int decode_offset = 0;
  CompiledMethod* nm = NULL;
  bool skip_fillInStackTrace_check = false;
  bool skip_throwableInit_check = false;
  bool skip_hidden = !ShowHiddenFrames;

  for (frame fr = thread->last_frame(); max_depth == 0 || max_depth != total_count;) {
    Method* method = NULL;
    int bci = 0;

    // Compiled java method case.
    if (decode_offset != 0) {
      DebugInfoReadStream stream(nm, decode_offset);
      decode_offset = stream.read_int();
      method = (Method*)nm->metadata_at(stream.read_int());
      bci = stream.read_bci();
    } else {
      if (fr.is_first_frame()) break;
      address pc = fr.pc();
      if (fr.is_interpreted_frame()) {
        address bcp = fr.interpreter_frame_bcp();
        method = fr.interpreter_frame_method();
        bci =  method->bci_from(bcp);
        fr = fr.sender(&map);
      } else {
        CodeBlob* cb = fr.cb();
        // HMMM QQQ might be nice to have frame return nm as NULL if cb is non-NULL
        // but non nmethod
        fr = fr.sender(&map);
        if (cb == NULL || !cb->is_compiled()) {
          continue;
        }
        nm = cb->as_compiled_method();
        if (nm->method()->is_native()) {
          method = nm->method();
          bci = 0;
        } else {
          PcDesc* pd = nm->pc_desc_at(pc);
          decode_offset = pd->scope_decode_offset();
          // if decode_offset is not equal to 0, it will execute the
          // "compiled java method case" at the beginning of the loop.
          continue;
        }
      }
    }
#ifdef ASSERT
    assert(st_method() == method && st.bci() == bci,
           "Wrong stack trace");
    st.next();
    // vframeStream::method isn't GC-safe so store off a copy
    // of the Method* in case we GC.
    if (!st.at_end()) {
      st_method = st.method();
    }
#endif

    // the format of the stacktrace will be:
    // - 1 or more fillInStackTrace frames for the exception class (skipped)
    // - 0 or more <init> methods for the exception class (skipped)
    // - rest of the stack

    if (!skip_fillInStackTrace_check) {
      if (method->name() == vmSymbols::fillInStackTrace_name() &&
          throwable->is_a(method->method_holder())) {
        continue;
      }
      else {
        skip_fillInStackTrace_check = true; // gone past them all
      }
    }
    if (!skip_throwableInit_check) {
      assert(skip_fillInStackTrace_check, "logic error in backtrace filtering");

      // skip <init> methods of the exception class and superclasses
      // This is simlar to classic VM.
      if (method->name() == vmSymbols::object_initializer_name() &&
          throwable->is_a(method->method_holder())) {
        continue;
      } else {
        // there are none or we've seen them all - either way stop checking
        skip_throwableInit_check = true;
      }
    }
    if (method->is_hidden()) {
      if (skip_hidden) {
        if (total_count == 0) {
          // The top frame will be hidden from the stack trace.
          bt.set_has_hidden_top_frame(CHECK);
        }
        continue;
      }
    }
    bt.push(method, bci, CHECK);
    total_count++;
  }

  log_info(stacktrace)("%s, %d", throwable->klass()->external_name(), total_count);

  // Put completed stack trace into throwable object
  set_backtrace(throwable(), bt.backtrace());
  set_depth(throwable(), total_count);
}

void java_lang_Throwable::fill_in_stack_trace(Handle throwable, const methodHandle& method) {
  // No-op if stack trace is disabled
  if (!StackTraceInThrowable) {
    return;
  }

  // Disable stack traces for some preallocated out of memory errors
  if (!Universe::should_fill_in_stack_trace(throwable)) {
    return;
  }

  PRESERVE_EXCEPTION_MARK;

  JavaThread* thread = JavaThread::active();
  fill_in_stack_trace(throwable, method, thread);
  // ignore exceptions thrown during stack trace filling
  CLEAR_PENDING_EXCEPTION;
}

void java_lang_Throwable::allocate_backtrace(Handle throwable, TRAPS) {
  // Allocate stack trace - backtrace is created but not filled in

  // No-op if stack trace is disabled
  if (!StackTraceInThrowable) return;
  BacktraceBuilder bt(CHECK);   // creates a backtrace
  set_backtrace(throwable(), bt.backtrace());
}


void java_lang_Throwable::fill_in_stack_trace_of_preallocated_backtrace(Handle throwable) {
  // Fill in stack trace into preallocated backtrace (no GC)

  // No-op if stack trace is disabled
  if (!StackTraceInThrowable) return;

  assert(throwable->is_a(SystemDictionary::Throwable_klass()), "sanity check");

  JavaThread* THREAD = JavaThread::current();

  objArrayHandle backtrace (THREAD, (objArrayOop)java_lang_Throwable::backtrace(throwable()));
  assert(backtrace.not_null(), "backtrace should have been preallocated");

  ResourceMark rm(THREAD);
  vframeStream st(THREAD);

  BacktraceBuilder bt(THREAD, backtrace);

  // Unlike fill_in_stack_trace we do not skip fillInStackTrace or throwable init
  // methods as preallocated errors aren't created by "java" code.

  // fill in as much stack trace as possible
  int chunk_count = 0;
  for (;!st.at_end(); st.next()) {
    bt.push(st.method(), st.bci(), CHECK);
    chunk_count++;

    // Bail-out for deep stacks
    if (chunk_count >= trace_chunk_size) break;
  }
  set_depth(throwable(), chunk_count);
  log_info(stacktrace)("%s, %d", throwable->klass()->external_name(), chunk_count);

  // We support the Throwable immutability protocol defined for Java 7.
  java_lang_Throwable::set_stacktrace(throwable(), java_lang_Throwable::unassigned_stacktrace());
  assert(java_lang_Throwable::unassigned_stacktrace() != NULL, "not initialized");
}

void java_lang_Throwable::get_stack_trace_elements(Handle throwable,
                                                   objArrayHandle stack_trace_array_h, TRAPS) {

  if (throwable.is_null() || stack_trace_array_h.is_null()) {
    THROW(vmSymbols::java_lang_NullPointerException());
  }

  assert(stack_trace_array_h->is_objArray(), "Stack trace array should be an array of StackTraceElenent");

  if (stack_trace_array_h->length() != depth(throwable())) {
    THROW(vmSymbols::java_lang_IndexOutOfBoundsException());
  }

  objArrayHandle result(THREAD, objArrayOop(backtrace(throwable())));
  BacktraceIterator iter(result, THREAD);

  int index = 0;
  while (iter.repeat()) {
    BacktraceElement bte = iter.next(THREAD);

    Handle stack_trace_element(THREAD, stack_trace_array_h->obj_at(index++));

    if (stack_trace_element.is_null()) {
      THROW(vmSymbols::java_lang_NullPointerException());
    }

    InstanceKlass* holder = InstanceKlass::cast(java_lang_Class::as_Klass(bte._mirror()));
    methodHandle method (THREAD, holder->method_with_orig_idnum(bte._method_id, bte._version));

    java_lang_StackTraceElement::fill_in(stack_trace_element, holder,
                                         method,
                                         bte._version,
                                         bte._bci,
                                         bte._name, CHECK);
  }
}

bool java_lang_Throwable::get_top_method_and_bci(oop throwable, Method** method, int* bci) {
  Thread* THREAD = Thread::current();
  objArrayHandle result(THREAD, objArrayOop(backtrace(throwable)));
  BacktraceIterator iter(result, THREAD);
  // No backtrace available.
  if (!iter.repeat()) return false;

  // If the exception happened in a frame that has been hidden, i.e.,
  // omitted from the back trace, we can not compute the message.
  oop hidden = ((objArrayOop)backtrace(throwable))->obj_at(trace_hidden_offset);
  if (hidden != NULL) {
    return false;
  }

  // Get first backtrace element.
  BacktraceElement bte = iter.next(THREAD);

  InstanceKlass* holder = InstanceKlass::cast(java_lang_Class::as_Klass(bte._mirror()));
  assert(holder != NULL, "first element should be non-null");
  Method* m = holder->method_with_orig_idnum(bte._method_id, bte._version);

  // Original version is no longer available.
  if (m == NULL || !version_matches(m, bte._version)) {
    return false;
  }

  *method = m;
  *bci = bte._bci;
  return true;
}

oop java_lang_StackTraceElement::create(const methodHandle& method, int bci, TRAPS) {
  // Allocate java.lang.StackTraceElement instance
  InstanceKlass* k = SystemDictionary::StackTraceElement_klass();
  assert(k != NULL, "must be loaded in 1.4+");
  if (k->should_be_initialized()) {
    k->initialize(CHECK_0);
  }

  Handle element = k->allocate_instance_handle(CHECK_0);

  int version = method->constants()->version();
  fill_in(element, method->method_holder(), method, version, bci, method->name(), CHECK_0);
  return element();
}

void java_lang_StackTraceElement::fill_in(Handle element,
                                          InstanceKlass* holder, const methodHandle& method,
                                          int version, int bci, Symbol* name, TRAPS) {
  assert(element->is_a(SystemDictionary::StackTraceElement_klass()), "sanity check");

  ResourceMark rm(THREAD);
  HandleMark hm(THREAD);

  // Fill in class name
  Handle java_class(THREAD, holder->java_mirror());
  oop classname = java_lang_Class::name(java_class, CHECK);
  java_lang_StackTraceElement::set_declaringClass(element(), classname);
  java_lang_StackTraceElement::set_declaringClassObject(element(), java_class());

  oop loader = holder->class_loader();
  if (loader != NULL) {
    oop loader_name = java_lang_ClassLoader::name(loader);
    if (loader_name != NULL)
      java_lang_StackTraceElement::set_classLoaderName(element(), loader_name);
  }

  // Fill in method name
  oop methodname = StringTable::intern(name, CHECK);
  java_lang_StackTraceElement::set_methodName(element(), methodname);

  // Fill in module name and version
  ModuleEntry* module = holder->module();
  if (module->is_named()) {
    oop module_name = StringTable::intern(module->name(), CHECK);
    java_lang_StackTraceElement::set_moduleName(element(), module_name);
    oop module_version;
    if (module->version() != NULL) {
      module_version = StringTable::intern(module->version(), CHECK);
    } else {
      module_version = NULL;
    }
    java_lang_StackTraceElement::set_moduleVersion(element(), module_version);
  }

  if (method() == NULL || !version_matches(method(), version)) {
    // The method was redefined, accurate line number information isn't available
    java_lang_StackTraceElement::set_fileName(element(), NULL);
    java_lang_StackTraceElement::set_lineNumber(element(), -1);
  } else {
    // Fill in source file name and line number.
    Symbol* source = Backtrace::get_source_file_name(holder, version);
    oop source_file = java_lang_Class::source_file(java_class());
    if (source != NULL) {
      // Class was not redefined. We can trust its cache if set,
      // else we have to initialize it.
      if (source_file == NULL) {
        source_file = StringTable::intern(source, CHECK);
        java_lang_Class::set_source_file(java_class(), source_file);
      }
    } else {
      // Class was redefined. Dump the cache if it was set.
      if (source_file != NULL) {
        source_file = NULL;
        java_lang_Class::set_source_file(java_class(), source_file);
      }
    }
    java_lang_StackTraceElement::set_fileName(element(), source_file);

    int line_number = Backtrace::get_line_number(method, bci);
    java_lang_StackTraceElement::set_lineNumber(element(), line_number);
  }
}

#if INCLUDE_JVMCI
void java_lang_StackTraceElement::decode(Handle mirror, methodHandle method, int bci, Symbol*& methodname, Symbol*& filename, int& line_number) {
  int method_id = method->orig_method_idnum();
  int cpref = method->name_index();
  decode(mirror, method_id, method->constants()->version(), bci, cpref, methodname, filename, line_number);
}

void java_lang_StackTraceElement::decode(Handle mirror, int method_id, int version, int bci, int cpref, Symbol*& methodname, Symbol*& filename, int& line_number) {
  // Fill in class name
  InstanceKlass* holder = InstanceKlass::cast(java_lang_Class::as_Klass(mirror()));
  Method* method = holder->method_with_orig_idnum(method_id, version);

  // The method can be NULL if the requested class version is gone
  Symbol* sym = (method != NULL) ? method->name() : holder->constants()->symbol_at(cpref);

  // Fill in method name
  methodname = sym;

  if (!version_matches(method, version)) {
    // If the method was redefined, accurate line number information isn't available
    filename = NULL;
    line_number = -1;
  } else {
    // Fill in source file name and line number.
    // Use a specific ik version as a holder since the mirror might
    // refer to a version that is now obsolete and no longer accessible
    // via the previous versions list.
    holder = holder->get_klass_version(version);
    assert(holder != NULL, "sanity check");
    filename = holder->source_file_name();
    line_number = Backtrace::get_line_number(method, bci);
  }
}
#endif // INCLUDE_JVMCI

Method* java_lang_StackFrameInfo::get_method(Handle stackFrame, InstanceKlass* holder, TRAPS) {
  HandleMark hm(THREAD);
  Handle mname(THREAD, stackFrame->obj_field(_memberName_offset));
  Method* method = (Method*)java_lang_invoke_MemberName::vmtarget(mname());
  // we should expand MemberName::name when Throwable uses StackTrace
  // MethodHandles::expand_MemberName(mname, MethodHandles::_suppress_defc|MethodHandles::_suppress_type, CHECK_NULL);
  return method;
}

void java_lang_StackFrameInfo::set_method_and_bci(Handle stackFrame, const methodHandle& method, int bci, TRAPS) {
  // set Method* or mid/cpref
  HandleMark hm(THREAD);
  Handle mname(Thread::current(), stackFrame->obj_field(_memberName_offset));
  InstanceKlass* ik = method->method_holder();
  CallInfo info(method(), ik, CHECK);
  MethodHandles::init_method_MemberName(mname, info);
  // set bci
  java_lang_StackFrameInfo::set_bci(stackFrame(), bci);
  // method may be redefined; store the version
  int version = method->constants()->version();
  assert((jushort)version == version, "version should be short");
  java_lang_StackFrameInfo::set_version(stackFrame(), (short)version);
}

void java_lang_StackFrameInfo::to_stack_trace_element(Handle stackFrame, Handle stack_trace_element, TRAPS) {
  ResourceMark rm(THREAD);
  HandleMark hm(THREAD);
  Handle mname(THREAD, stackFrame->obj_field(java_lang_StackFrameInfo::_memberName_offset));
  Klass* clazz = java_lang_Class::as_Klass(java_lang_invoke_MemberName::clazz(mname()));
  InstanceKlass* holder = InstanceKlass::cast(clazz);
  Method* method = java_lang_StackFrameInfo::get_method(stackFrame, holder, CHECK);

  short version = stackFrame->short_field(_version_offset);
  int bci = stackFrame->int_field(_bci_offset);
  Symbol* name = method->name();
  java_lang_StackTraceElement::fill_in(stack_trace_element, holder, method, version, bci, name, CHECK);
}

#define STACKFRAMEINFO_FIELDS_DO(macro) \
  macro(_memberName_offset,     k, "memberName",  object_signature, false); \
  macro(_bci_offset,            k, "bci",         int_signature,    false)

void java_lang_StackFrameInfo::compute_offsets() {
  InstanceKlass* k = SystemDictionary::StackFrameInfo_klass();
  STACKFRAMEINFO_FIELDS_DO(FIELD_COMPUTE_OFFSET);
  STACKFRAMEINFO_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_StackFrameInfo::serialize_offsets(SerializeClosure* f) {
  STACKFRAMEINFO_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
  STACKFRAMEINFO_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

#define LIVESTACKFRAMEINFO_FIELDS_DO(macro) \
  macro(_monitors_offset,   k, "monitors",    object_array_signature, false); \
  macro(_locals_offset,     k, "locals",      object_array_signature, false); \
  macro(_operands_offset,   k, "operands",    object_array_signature, false); \
  macro(_mode_offset,       k, "mode",        int_signature,          false)

void java_lang_LiveStackFrameInfo::compute_offsets() {
  InstanceKlass* k = SystemDictionary::LiveStackFrameInfo_klass();
  LIVESTACKFRAMEINFO_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_LiveStackFrameInfo::serialize_offsets(SerializeClosure* f) {
  LIVESTACKFRAMEINFO_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define ACCESSIBLEOBJECT_FIELDS_DO(macro) \
  macro(override_offset, k, "override", bool_signature, false)

void java_lang_reflect_AccessibleObject::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_AccessibleObject_klass();
  ACCESSIBLEOBJECT_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_AccessibleObject::serialize_offsets(SerializeClosure* f) {
  ACCESSIBLEOBJECT_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

jboolean java_lang_reflect_AccessibleObject::override(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return (jboolean) reflect->bool_field(override_offset);
}

void java_lang_reflect_AccessibleObject::set_override(oop reflect, jboolean value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  reflect->bool_field_put(override_offset, (int) value);
}

#define METHOD_FIELDS_DO(macro) \
  macro(clazz_offset,          k, vmSymbols::clazz_name(),          class_signature,       false); \
  macro(name_offset,           k, vmSymbols::name_name(),           string_signature,      false); \
  macro(returnType_offset,     k, vmSymbols::returnType_name(),     class_signature,       false); \
  macro(parameterTypes_offset, k, vmSymbols::parameterTypes_name(), class_array_signature, false); \
  macro(exceptionTypes_offset, k, vmSymbols::exceptionTypes_name(), class_array_signature, false); \
  macro(slot_offset,           k, vmSymbols::slot_name(),           int_signature,         false); \
  macro(modifiers_offset,      k, vmSymbols::modifiers_name(),      int_signature,         false); \
  macro(signature_offset,             k, vmSymbols::signature_name(),             string_signature,     false); \
  macro(annotations_offset,           k, vmSymbols::annotations_name(),           byte_array_signature, false); \
  macro(parameter_annotations_offset, k, vmSymbols::parameter_annotations_name(), byte_array_signature, false); \
  macro(annotation_default_offset,    k, vmSymbols::annotation_default_name(),    byte_array_signature, false);

void java_lang_reflect_Method::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_Method_klass();
  METHOD_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_Method::serialize_offsets(SerializeClosure* f) {
  METHOD_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

Handle java_lang_reflect_Method::create(TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  Klass* klass = SystemDictionary::reflect_Method_klass();
  // This class is eagerly initialized during VM initialization, since we keep a refence
  // to one of the methods
  assert(InstanceKlass::cast(klass)->is_initialized(), "must be initialized");
  return InstanceKlass::cast(klass)->allocate_instance_handle(THREAD);
}

oop java_lang_reflect_Method::clazz(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->obj_field(clazz_offset);
}

void java_lang_reflect_Method::set_clazz(oop reflect, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
   reflect->obj_field_put(clazz_offset, value);
}

int java_lang_reflect_Method::slot(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->int_field(slot_offset);
}

void java_lang_reflect_Method::set_slot(oop reflect, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  reflect->int_field_put(slot_offset, value);
}

void java_lang_reflect_Method::set_name(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(name_offset, value);
}

oop java_lang_reflect_Method::return_type(oop method) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return method->obj_field(returnType_offset);
}

void java_lang_reflect_Method::set_return_type(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(returnType_offset, value);
}

oop java_lang_reflect_Method::parameter_types(oop method) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return method->obj_field(parameterTypes_offset);
}

void java_lang_reflect_Method::set_parameter_types(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(parameterTypes_offset, value);
}

void java_lang_reflect_Method::set_exception_types(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(exceptionTypes_offset, value);
}

void java_lang_reflect_Method::set_modifiers(oop method, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->int_field_put(modifiers_offset, value);
}

void java_lang_reflect_Method::set_signature(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(signature_offset, value);
}

void java_lang_reflect_Method::set_annotations(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(annotations_offset, value);
}

void java_lang_reflect_Method::set_parameter_annotations(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(parameter_annotations_offset, value);
}

void java_lang_reflect_Method::set_annotation_default(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(annotation_default_offset, value);
}

#define CONSTRUCTOR_FIELDS_DO(macro) \
  macro(clazz_offset,          k, vmSymbols::clazz_name(),          class_signature,       false); \
  macro(parameterTypes_offset, k, vmSymbols::parameterTypes_name(), class_array_signature, false); \
  macro(exceptionTypes_offset, k, vmSymbols::exceptionTypes_name(), class_array_signature, false); \
  macro(slot_offset,           k, vmSymbols::slot_name(),           int_signature,         false); \
  macro(modifiers_offset,      k, vmSymbols::modifiers_name(),      int_signature,         false); \
  macro(signature_offset,             k, vmSymbols::signature_name(),             string_signature,     false); \
  macro(annotations_offset,           k, vmSymbols::annotations_name(),           byte_array_signature, false); \
  macro(parameter_annotations_offset, k, vmSymbols::parameter_annotations_name(), byte_array_signature, false);

void java_lang_reflect_Constructor::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_Constructor_klass();
  CONSTRUCTOR_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_Constructor::serialize_offsets(SerializeClosure* f) {
  CONSTRUCTOR_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

Handle java_lang_reflect_Constructor::create(TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  Symbol* name = vmSymbols::java_lang_reflect_Constructor();
  Klass* k = SystemDictionary::resolve_or_fail(name, true, CHECK_NH);
  InstanceKlass* ik = InstanceKlass::cast(k);
  // Ensure it is initialized
  ik->initialize(CHECK_NH);
  return ik->allocate_instance_handle(THREAD);
}

oop java_lang_reflect_Constructor::clazz(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->obj_field(clazz_offset);
}

void java_lang_reflect_Constructor::set_clazz(oop reflect, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
   reflect->obj_field_put(clazz_offset, value);
}

oop java_lang_reflect_Constructor::parameter_types(oop constructor) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return constructor->obj_field(parameterTypes_offset);
}

void java_lang_reflect_Constructor::set_parameter_types(oop constructor, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  constructor->obj_field_put(parameterTypes_offset, value);
}

void java_lang_reflect_Constructor::set_exception_types(oop constructor, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  constructor->obj_field_put(exceptionTypes_offset, value);
}

int java_lang_reflect_Constructor::slot(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->int_field(slot_offset);
}

void java_lang_reflect_Constructor::set_slot(oop reflect, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  reflect->int_field_put(slot_offset, value);
}

void java_lang_reflect_Constructor::set_modifiers(oop constructor, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  constructor->int_field_put(modifiers_offset, value);
}

void java_lang_reflect_Constructor::set_signature(oop constructor, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  constructor->obj_field_put(signature_offset, value);
}

void java_lang_reflect_Constructor::set_annotations(oop constructor, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  constructor->obj_field_put(annotations_offset, value);
}

void java_lang_reflect_Constructor::set_parameter_annotations(oop method, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  method->obj_field_put(parameter_annotations_offset, value);
}

#define FIELD_FIELDS_DO(macro) \
  macro(clazz_offset,     k, vmSymbols::clazz_name(),     class_signature,  false); \
  macro(name_offset,      k, vmSymbols::name_name(),      string_signature, false); \
  macro(type_offset,      k, vmSymbols::type_name(),      class_signature,  false); \
  macro(slot_offset,      k, vmSymbols::slot_name(),      int_signature,    false); \
  macro(modifiers_offset, k, vmSymbols::modifiers_name(), int_signature,    false); \
  macro(signature_offset,        k, vmSymbols::signature_name(),        string_signature,     false); \
  macro(annotations_offset,      k, vmSymbols::annotations_name(),      byte_array_signature, false);

void java_lang_reflect_Field::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_Field_klass();
  FIELD_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_Field::serialize_offsets(SerializeClosure* f) {
  FIELD_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

Handle java_lang_reflect_Field::create(TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  Symbol* name = vmSymbols::java_lang_reflect_Field();
  Klass* k = SystemDictionary::resolve_or_fail(name, true, CHECK_NH);
  InstanceKlass* ik = InstanceKlass::cast(k);
  // Ensure it is initialized
  ik->initialize(CHECK_NH);
  return ik->allocate_instance_handle(THREAD);
}

oop java_lang_reflect_Field::clazz(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->obj_field(clazz_offset);
}

void java_lang_reflect_Field::set_clazz(oop reflect, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
   reflect->obj_field_put(clazz_offset, value);
}

oop java_lang_reflect_Field::name(oop field) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return field->obj_field(name_offset);
}

void java_lang_reflect_Field::set_name(oop field, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  field->obj_field_put(name_offset, value);
}

oop java_lang_reflect_Field::type(oop field) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return field->obj_field(type_offset);
}

void java_lang_reflect_Field::set_type(oop field, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  field->obj_field_put(type_offset, value);
}

int java_lang_reflect_Field::slot(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return reflect->int_field(slot_offset);
}

void java_lang_reflect_Field::set_slot(oop reflect, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  reflect->int_field_put(slot_offset, value);
}

int java_lang_reflect_Field::modifiers(oop field) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return field->int_field(modifiers_offset);
}

void java_lang_reflect_Field::set_modifiers(oop field, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  field->int_field_put(modifiers_offset, value);
}

void java_lang_reflect_Field::set_signature(oop field, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  field->obj_field_put(signature_offset, value);
}

void java_lang_reflect_Field::set_annotations(oop field, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  field->obj_field_put(annotations_offset, value);
}

oop java_lang_reflect_RecordComponent::create(InstanceKlass* holder, RecordComponent* component, TRAPS) {
  // Allocate java.lang.reflect.RecordComponent instance
  HandleMark hm(THREAD);
  InstanceKlass* ik = SystemDictionary::RecordComponent_klass();
  assert(ik != NULL, "must be loaded");
  if (ik->should_be_initialized()) {
    ik->initialize(CHECK_0);
  }

  Handle element = ik->allocate_instance_handle(CHECK_0);

  Handle decl_class(THREAD, holder->java_mirror());
  java_lang_reflect_RecordComponent::set_clazz(element(), decl_class());

  Symbol* name = holder->constants()->symbol_at(component->name_index()); // name_index is a utf8
  oop component_name = StringTable::intern(name, CHECK_0);
  java_lang_reflect_RecordComponent::set_name(element(), component_name);

  Symbol* type = holder->constants()->symbol_at(component->descriptor_index());
  Handle component_type_h =
    SystemDictionary::find_java_mirror_for_type(type, holder, SignatureStream::NCDFError, CHECK_0);
  java_lang_reflect_RecordComponent::set_type(element(), component_type_h());

  Method* accessor_method = NULL;
  {
    // Prepend "()" to type to create the full method signature.
    ResourceMark rm(THREAD);
    int sig_len = type->utf8_length() + 3; // "()" and null char
    char* sig = NEW_RESOURCE_ARRAY(char, sig_len);
    jio_snprintf(sig, sig_len, "()%s", type->as_C_string());
    TempNewSymbol full_sig = SymbolTable::new_symbol(sig);
    accessor_method = holder->find_instance_method(name, full_sig);
  }

  if (accessor_method != NULL) {
    methodHandle method(THREAD, accessor_method);
    oop m = Reflection::new_method(method, false, CHECK_0);
    java_lang_reflect_RecordComponent::set_accessor(element(), m);
  } else {
    java_lang_reflect_RecordComponent::set_accessor(element(), NULL);
  }

  int sig_index = component->generic_signature_index();
  if (sig_index > 0) {
    Symbol* sig = holder->constants()->symbol_at(sig_index); // sig_index is a utf8
    oop component_sig = StringTable::intern(sig, CHECK_0);
    java_lang_reflect_RecordComponent::set_signature(element(), component_sig);
  } else {
    java_lang_reflect_RecordComponent::set_signature(element(), NULL);
  }

  typeArrayOop annotation_oop = Annotations::make_java_array(component->annotations(), CHECK_0);
  java_lang_reflect_RecordComponent::set_annotations(element(), annotation_oop);

  typeArrayOop type_annotation_oop = Annotations::make_java_array(component->type_annotations(), CHECK_0);
  java_lang_reflect_RecordComponent::set_typeAnnotations(element(), type_annotation_oop);

  return element();
}

#define CONSTANTPOOL_FIELDS_DO(macro) \
  macro(_oop_offset, k, "constantPoolOop", object_signature, false)

void reflect_ConstantPool::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_ConstantPool_klass();
  // The field is called ConstantPool* in the sun.reflect.ConstantPool class.
  CONSTANTPOOL_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void reflect_ConstantPool::serialize_offsets(SerializeClosure* f) {
  CONSTANTPOOL_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define PARAMETER_FIELDS_DO(macro) \
  macro(name_offset,        k, vmSymbols::name_name(),        string_signature, false); \
  macro(modifiers_offset,   k, vmSymbols::modifiers_name(),   int_signature,    false); \
  macro(index_offset,       k, vmSymbols::index_name(),       int_signature,    false); \
  macro(executable_offset,  k, vmSymbols::executable_name(),  executable_signature, false)

void java_lang_reflect_Parameter::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_Parameter_klass();
  PARAMETER_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_Parameter::serialize_offsets(SerializeClosure* f) {
  PARAMETER_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

Handle java_lang_reflect_Parameter::create(TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  Symbol* name = vmSymbols::java_lang_reflect_Parameter();
  Klass* k = SystemDictionary::resolve_or_fail(name, true, CHECK_NH);
  InstanceKlass* ik = InstanceKlass::cast(k);
  // Ensure it is initialized
  ik->initialize(CHECK_NH);
  return ik->allocate_instance_handle(THREAD);
}

oop java_lang_reflect_Parameter::name(oop param) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return param->obj_field(name_offset);
}

void java_lang_reflect_Parameter::set_name(oop param, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  param->obj_field_put(name_offset, value);
}

int java_lang_reflect_Parameter::modifiers(oop param) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return param->int_field(modifiers_offset);
}

void java_lang_reflect_Parameter::set_modifiers(oop param, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  param->int_field_put(modifiers_offset, value);
}

int java_lang_reflect_Parameter::index(oop param) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return param->int_field(index_offset);
}

void java_lang_reflect_Parameter::set_index(oop param, int value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  param->int_field_put(index_offset, value);
}

oop java_lang_reflect_Parameter::executable(oop param) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return param->obj_field(executable_offset);
}

void java_lang_reflect_Parameter::set_executable(oop param, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  param->obj_field_put(executable_offset, value);
}


int java_lang_Module::loader_offset;
int java_lang_Module::name_offset;
int java_lang_Module::_module_entry_offset = -1;

Handle java_lang_Module::create(Handle loader, Handle module_name, TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return JavaCalls::construct_new_instance(SystemDictionary::Module_klass(),
                          vmSymbols::java_lang_module_init_signature(),
                          loader, module_name, CHECK_NH);
}

#define MODULE_FIELDS_DO(macro) \
  macro(loader_offset,  k, vmSymbols::loader_name(),  classloader_signature, false); \
  macro(name_offset,    k, vmSymbols::name_name(),    string_signature,      false)

void java_lang_Module::compute_offsets() {
  InstanceKlass* k = SystemDictionary::Module_klass();
  MODULE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
  MODULE_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_Module::serialize_offsets(SerializeClosure* f) {
  MODULE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
  MODULE_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_lang_Module::loader(oop module) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return module->obj_field(loader_offset);
}

void java_lang_Module::set_loader(oop module, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  module->obj_field_put(loader_offset, value);
}

oop java_lang_Module::name(oop module) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  return module->obj_field(name_offset);
}

void java_lang_Module::set_name(oop module, oop value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  module->obj_field_put(name_offset, value);
}

ModuleEntry* java_lang_Module::module_entry(oop module) {
  assert(_module_entry_offset != -1, "Uninitialized module_entry_offset");
  assert(module != NULL, "module can't be null");
  assert(oopDesc::is_oop(module), "module must be oop");

  ModuleEntry* module_entry = (ModuleEntry*)module->address_field(_module_entry_offset);
  if (module_entry == NULL) {
    // If the inject field containing the ModuleEntry* is null then return the
    // class loader's unnamed module.
    oop loader = java_lang_Module::loader(module);
    Handle h_loader = Handle(Thread::current(), loader);
    ClassLoaderData* loader_cld = SystemDictionary::register_loader(h_loader);
    return loader_cld->unnamed_module();
  }
  return module_entry;
}

void java_lang_Module::set_module_entry(oop module, ModuleEntry* module_entry) {
  assert(_module_entry_offset != -1, "Uninitialized module_entry_offset");
  assert(module != NULL, "module can't be null");
  assert(oopDesc::is_oop(module), "module must be oop");
  module->address_field_put(_module_entry_offset, (address)module_entry);
}

Handle reflect_ConstantPool::create(TRAPS) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  InstanceKlass* k = SystemDictionary::reflect_ConstantPool_klass();
  // Ensure it is initialized
  k->initialize(CHECK_NH);
  return k->allocate_instance_handle(THREAD);
}


void reflect_ConstantPool::set_cp(oop reflect, ConstantPool* value) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");
  oop mirror = value->pool_holder()->java_mirror();
  // Save the mirror to get back the constant pool.
  reflect->obj_field_put(_oop_offset, mirror);
}

ConstantPool* reflect_ConstantPool::get_cp(oop reflect) {
  assert(Universe::is_fully_initialized(), "Need to find another solution to the reflection problem");

  oop mirror = reflect->obj_field(_oop_offset);
  Klass* k = java_lang_Class::as_Klass(mirror);
  assert(k->is_instance_klass(), "Must be");

  // Get the constant pool back from the klass.  Since class redefinition
  // merges the new constant pool into the old, this is essentially the
  // same constant pool as the original.  If constant pool merging is
  // no longer done in the future, this will have to change to save
  // the original.
  return InstanceKlass::cast(k)->constants();
}

#define UNSAFESTATICFIELDACCESSORIMPL_FIELDS_DO(macro) \
  macro(_base_offset, k, "base", object_signature, false)

void reflect_UnsafeStaticFieldAccessorImpl::compute_offsets() {
  InstanceKlass* k = SystemDictionary::reflect_UnsafeStaticFieldAccessorImpl_klass();
  UNSAFESTATICFIELDACCESSORIMPL_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void reflect_UnsafeStaticFieldAccessorImpl::serialize_offsets(SerializeClosure* f) {
  UNSAFESTATICFIELDACCESSORIMPL_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_lang_boxing_object::initialize_and_allocate(BasicType type, TRAPS) {
  Klass* k = SystemDictionary::box_klass(type);
  if (k == NULL)  return NULL;
  InstanceKlass* ik = InstanceKlass::cast(k);
  if (!ik->is_initialized())  ik->initialize(CHECK_0);
  return ik->allocate_instance(THREAD);
}


oop java_lang_boxing_object::create(BasicType type, jvalue* value, TRAPS) {
  oop box = initialize_and_allocate(type, CHECK_0);
  if (box == NULL)  return NULL;
  switch (type) {
    case T_BOOLEAN:
      box->bool_field_put(value_offset, value->z);
      break;
    case T_CHAR:
      box->char_field_put(value_offset, value->c);
      break;
    case T_FLOAT:
      box->float_field_put(value_offset, value->f);
      break;
    case T_DOUBLE:
      box->double_field_put(long_value_offset, value->d);
      break;
    case T_BYTE:
      box->byte_field_put(value_offset, value->b);
      break;
    case T_SHORT:
      box->short_field_put(value_offset, value->s);
      break;
    case T_INT:
      box->int_field_put(value_offset, value->i);
      break;
    case T_LONG:
      box->long_field_put(long_value_offset, value->j);
      break;
    default:
      return NULL;
  }
  return box;
}


BasicType java_lang_boxing_object::basic_type(oop box) {
  if (box == NULL)  return T_ILLEGAL;
  BasicType type = SystemDictionary::box_klass_type(box->klass());
  if (type == T_OBJECT)         // 'unknown' value returned by SD::bkt
    return T_ILLEGAL;
  return type;
}


BasicType java_lang_boxing_object::get_value(oop box, jvalue* value) {
  BasicType type = SystemDictionary::box_klass_type(box->klass());
  switch (type) {
  case T_BOOLEAN:
    value->z = box->bool_field(value_offset);
    break;
  case T_CHAR:
    value->c = box->char_field(value_offset);
    break;
  case T_FLOAT:
    value->f = box->float_field(value_offset);
    break;
  case T_DOUBLE:
    value->d = box->double_field(long_value_offset);
    break;
  case T_BYTE:
    value->b = box->byte_field(value_offset);
    break;
  case T_SHORT:
    value->s = box->short_field(value_offset);
    break;
  case T_INT:
    value->i = box->int_field(value_offset);
    break;
  case T_LONG:
    value->j = box->long_field(long_value_offset);
    break;
  default:
    return T_ILLEGAL;
  } // end switch
  return type;
}


BasicType java_lang_boxing_object::set_value(oop box, jvalue* value) {
  BasicType type = SystemDictionary::box_klass_type(box->klass());
  switch (type) {
  case T_BOOLEAN:
    box->bool_field_put(value_offset, value->z);
    break;
  case T_CHAR:
    box->char_field_put(value_offset, value->c);
    break;
  case T_FLOAT:
    box->float_field_put(value_offset, value->f);
    break;
  case T_DOUBLE:
    box->double_field_put(long_value_offset, value->d);
    break;
  case T_BYTE:
    box->byte_field_put(value_offset, value->b);
    break;
  case T_SHORT:
    box->short_field_put(value_offset, value->s);
    break;
  case T_INT:
    box->int_field_put(value_offset, value->i);
    break;
  case T_LONG:
    box->long_field_put(long_value_offset, value->j);
    break;
  default:
    return T_ILLEGAL;
  } // end switch
  return type;
}


void java_lang_boxing_object::print(BasicType type, jvalue* value, outputStream* st) {
  switch (type) {
  case T_BOOLEAN:   st->print("%s", value->z ? "true" : "false");   break;
  case T_CHAR:      st->print("%d", value->c);                      break;
  case T_BYTE:      st->print("%d", value->b);                      break;
  case T_SHORT:     st->print("%d", value->s);                      break;
  case T_INT:       st->print("%d", value->i);                      break;
  case T_LONG:      st->print(JLONG_FORMAT, value->j);              break;
  case T_FLOAT:     st->print("%f", value->f);                      break;
  case T_DOUBLE:    st->print("%lf", value->d);                     break;
  default:          st->print("type %d?", type);                    break;
  }
}

// Support for java_lang_ref_Reference

bool java_lang_ref_Reference::is_referent_field(oop obj, ptrdiff_t offset) {
  assert(obj != NULL, "sanity");
  if (offset != java_lang_ref_Reference::referent_offset) {
    return false;
  }

  Klass* k = obj->klass();
  if (!k->is_instance_klass()) {
    return false;
  }

  InstanceKlass* ik = InstanceKlass::cast(obj->klass());
  bool is_reference = ik->reference_type() != REF_NONE;
  assert(!is_reference || ik->is_subclass_of(SystemDictionary::Reference_klass()), "sanity");
  return is_reference;
}

// Support for java_lang_ref_SoftReference
//

#define SOFTREFERENCE_FIELDS_DO(macro) \
  macro(timestamp_offset,    k, "timestamp", long_signature, false); \
  macro(static_clock_offset, k, "clock",     long_signature, true)

void java_lang_ref_SoftReference::compute_offsets() {
  InstanceKlass* k = SystemDictionary::SoftReference_klass();
  SOFTREFERENCE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_ref_SoftReference::serialize_offsets(SerializeClosure* f) {
  SOFTREFERENCE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

jlong java_lang_ref_SoftReference::timestamp(oop ref) {
  return ref->long_field(timestamp_offset);
}

jlong java_lang_ref_SoftReference::clock() {
  InstanceKlass* ik = SystemDictionary::SoftReference_klass();
  oop base = ik->static_field_base_raw();
  return base->long_field(static_clock_offset);
}

void java_lang_ref_SoftReference::set_clock(jlong value) {
  InstanceKlass* ik = SystemDictionary::SoftReference_klass();
  oop base = ik->static_field_base_raw();
  base->long_field_put(static_clock_offset, value);
}

// Support for java_lang_invoke_DirectMethodHandle

int java_lang_invoke_DirectMethodHandle::_member_offset;

oop java_lang_invoke_DirectMethodHandle::member(oop dmh) {
  oop member_name = NULL;
  assert(oopDesc::is_oop(dmh) && java_lang_invoke_DirectMethodHandle::is_instance(dmh),
         "a DirectMethodHandle oop is expected");
  return dmh->obj_field(member_offset_in_bytes());
}

#define DIRECTMETHODHANDLE_FIELDS_DO(macro) \
  macro(_member_offset, k, "member", java_lang_invoke_MemberName_signature, false)

void java_lang_invoke_DirectMethodHandle::compute_offsets() {
  InstanceKlass* k = SystemDictionary::DirectMethodHandle_klass();
  DIRECTMETHODHANDLE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_DirectMethodHandle::serialize_offsets(SerializeClosure* f) {
  DIRECTMETHODHANDLE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

// Support for java_lang_invoke_MethodHandle

int java_lang_invoke_MethodHandle::_type_offset;
int java_lang_invoke_MethodHandle::_form_offset;

int java_lang_invoke_MemberName::_clazz_offset;
int java_lang_invoke_MemberName::_name_offset;
int java_lang_invoke_MemberName::_type_offset;
int java_lang_invoke_MemberName::_flags_offset;
int java_lang_invoke_MemberName::_method_offset;
int java_lang_invoke_MemberName::_vmindex_offset;

int java_lang_invoke_ResolvedMethodName::_vmtarget_offset;
int java_lang_invoke_ResolvedMethodName::_vmholder_offset;

int java_lang_invoke_LambdaForm::_vmentry_offset;

#define METHODHANDLE_FIELDS_DO(macro) \
  macro(_type_offset, k, vmSymbols::type_name(), java_lang_invoke_MethodType_signature, false); \
  macro(_form_offset, k, "form",                 java_lang_invoke_LambdaForm_signature, false)

void java_lang_invoke_MethodHandle::compute_offsets() {
  InstanceKlass* k = SystemDictionary::MethodHandle_klass();
  METHODHANDLE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_MethodHandle::serialize_offsets(SerializeClosure* f) {
  METHODHANDLE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define MEMBERNAME_FIELDS_DO(macro) \
  macro(_clazz_offset,   k, vmSymbols::clazz_name(),   class_signature,  false); \
  macro(_name_offset,    k, vmSymbols::name_name(),    string_signature, false); \
  macro(_type_offset,    k, vmSymbols::type_name(),    object_signature, false); \
  macro(_flags_offset,   k, vmSymbols::flags_name(),   int_signature,    false); \
  macro(_method_offset,  k, vmSymbols::method_name(),  java_lang_invoke_ResolvedMethodName_signature, false)

void java_lang_invoke_MemberName::compute_offsets() {
  InstanceKlass* k = SystemDictionary::MemberName_klass();
  MEMBERNAME_FIELDS_DO(FIELD_COMPUTE_OFFSET);
  MEMBERNAME_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_MemberName::serialize_offsets(SerializeClosure* f) {
  MEMBERNAME_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
  MEMBERNAME_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

void java_lang_invoke_ResolvedMethodName::compute_offsets() {
  InstanceKlass* k = SystemDictionary::ResolvedMethodName_klass();
  assert(k != NULL, "jdk mismatch");
  RESOLVEDMETHOD_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_ResolvedMethodName::serialize_offsets(SerializeClosure* f) {
  RESOLVEDMETHOD_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

#define LAMBDAFORM_FIELDS_DO(macro) \
  macro(_vmentry_offset, k, "vmentry", java_lang_invoke_MemberName_signature, false)

void java_lang_invoke_LambdaForm::compute_offsets() {
  InstanceKlass* k = SystemDictionary::LambdaForm_klass();
  assert (k != NULL, "jdk mismatch");
  LAMBDAFORM_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_LambdaForm::serialize_offsets(SerializeClosure* f) {
  LAMBDAFORM_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

bool java_lang_invoke_LambdaForm::is_instance(oop obj) {
  return obj != NULL && is_subclass(obj->klass());
}


oop java_lang_invoke_MethodHandle::type(oop mh) {
  return mh->obj_field(_type_offset);
}

void java_lang_invoke_MethodHandle::set_type(oop mh, oop mtype) {
  mh->obj_field_put(_type_offset, mtype);
}

oop java_lang_invoke_MethodHandle::form(oop mh) {
  assert(_form_offset != 0, "");
  return mh->obj_field(_form_offset);
}

void java_lang_invoke_MethodHandle::set_form(oop mh, oop lform) {
  assert(_form_offset != 0, "");
  mh->obj_field_put(_form_offset, lform);
}

/// MemberName accessors

oop java_lang_invoke_MemberName::clazz(oop mname) {
  assert(is_instance(mname), "wrong type");
  return mname->obj_field(_clazz_offset);
}

void java_lang_invoke_MemberName::set_clazz(oop mname, oop clazz) {
  assert(is_instance(mname), "wrong type");
  mname->obj_field_put(_clazz_offset, clazz);
}

oop java_lang_invoke_MemberName::name(oop mname) {
  assert(is_instance(mname), "wrong type");
  return mname->obj_field(_name_offset);
}

void java_lang_invoke_MemberName::set_name(oop mname, oop name) {
  assert(is_instance(mname), "wrong type");
  mname->obj_field_put(_name_offset, name);
}

oop java_lang_invoke_MemberName::type(oop mname) {
  assert(is_instance(mname), "wrong type");
  return mname->obj_field(_type_offset);
}

void java_lang_invoke_MemberName::set_type(oop mname, oop type) {
  assert(is_instance(mname), "wrong type");
  mname->obj_field_put(_type_offset, type);
}

int java_lang_invoke_MemberName::flags(oop mname) {
  assert(is_instance(mname), "wrong type");
  return mname->int_field(_flags_offset);
}

void java_lang_invoke_MemberName::set_flags(oop mname, int flags) {
  assert(is_instance(mname), "wrong type");
  mname->int_field_put(_flags_offset, flags);
}


// Return vmtarget from ResolvedMethodName method field through indirection
Method* java_lang_invoke_MemberName::vmtarget(oop mname) {
  assert(is_instance(mname), "wrong type");
  oop method = mname->obj_field(_method_offset);
  return method == NULL ? NULL : java_lang_invoke_ResolvedMethodName::vmtarget(method);
}

bool java_lang_invoke_MemberName::is_method(oop mname) {
  assert(is_instance(mname), "must be MemberName");
  return (flags(mname) & (MN_IS_METHOD | MN_IS_CONSTRUCTOR)) > 0;
}

void java_lang_invoke_MemberName::set_method(oop mname, oop resolved_method) {
  assert(is_instance(mname), "wrong type");
  mname->obj_field_put(_method_offset, resolved_method);
}

intptr_t java_lang_invoke_MemberName::vmindex(oop mname) {
  assert(is_instance(mname), "wrong type");
  return (intptr_t) mname->address_field(_vmindex_offset);
}

void java_lang_invoke_MemberName::set_vmindex(oop mname, intptr_t index) {
  assert(is_instance(mname), "wrong type");
  mname->address_field_put(_vmindex_offset, (address) index);
}


Method* java_lang_invoke_ResolvedMethodName::vmtarget(oop resolved_method) {
  assert(is_instance(resolved_method), "wrong type");
  Method* m = (Method*)resolved_method->address_field(_vmtarget_offset);
  assert(m->is_method(), "must be");
  return m;
}

// Used by redefinition to change Method* to new Method* with same hash (name, signature)
void java_lang_invoke_ResolvedMethodName::set_vmtarget(oop resolved_method, Method* m) {
  assert(is_instance(resolved_method), "wrong type");
  resolved_method->address_field_put(_vmtarget_offset, (address)m);
}

void java_lang_invoke_ResolvedMethodName::set_vmholder(oop resolved_method, oop holder) {
  assert(is_instance(resolved_method), "wrong type");
  resolved_method->obj_field_put(_vmholder_offset, holder);
}

oop java_lang_invoke_ResolvedMethodName::find_resolved_method(const methodHandle& m, TRAPS) {
  const Method* method = m();

  // lookup ResolvedMethod oop in the table, or create a new one and intern it
  oop resolved_method = ResolvedMethodTable::find_method(method);
  if (resolved_method != NULL) {
    return resolved_method;
  }

  InstanceKlass* k = SystemDictionary::ResolvedMethodName_klass();
  if (!k->is_initialized()) {
    k->initialize(CHECK_NULL);
  }

  oop new_resolved_method = k->allocate_instance(CHECK_NULL);

  NoSafepointVerifier nsv;

  if (method->is_old()) {
    method = (method->is_deleted()) ? Universe::throw_no_such_method_error() :
                                      method->get_new_method();
  }

  InstanceKlass* holder = method->method_holder();

  set_vmtarget(new_resolved_method, const_cast<Method*>(method));
  // Add a reference to the loader (actually mirror because unsafe anonymous classes will not have
  // distinct loaders) to ensure the metadata is kept alive.
  // This mirror may be different than the one in clazz field.
  set_vmholder(new_resolved_method, holder->java_mirror());

  // Set flag in class to indicate this InstanceKlass has entries in the table
  // to avoid walking table during redefinition if none of the redefined classes
  // have any membernames in the table.
  holder->set_has_resolved_methods();

  return ResolvedMethodTable::add_method(method, Handle(THREAD, new_resolved_method));
}

oop java_lang_invoke_LambdaForm::vmentry(oop lform) {
  assert(is_instance(lform), "wrong type");
  return lform->obj_field(_vmentry_offset);
}


// Support for java_lang_invoke_MethodType

int java_lang_invoke_MethodType::_rtype_offset;
int java_lang_invoke_MethodType::_ptypes_offset;

#define METHODTYPE_FIELDS_DO(macro) \
  macro(_rtype_offset,  k, "rtype",  class_signature,       false); \
  macro(_ptypes_offset, k, "ptypes", class_array_signature, false)

void java_lang_invoke_MethodType::compute_offsets() {
  InstanceKlass* k = SystemDictionary::MethodType_klass();
  METHODTYPE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_MethodType::serialize_offsets(SerializeClosure* f) {
  METHODTYPE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

void java_lang_invoke_MethodType::print_signature(oop mt, outputStream* st) {
  st->print("(");
  objArrayOop pts = ptypes(mt);
  for (int i = 0, limit = pts->length(); i < limit; i++) {
    java_lang_Class::print_signature(pts->obj_at(i), st);
  }
  st->print(")");
  java_lang_Class::print_signature(rtype(mt), st);
}

Symbol* java_lang_invoke_MethodType::as_signature(oop mt, bool intern_if_not_found) {
  ResourceMark rm;
  stringStream buffer(128);
  print_signature(mt, &buffer);
  const char* sigstr =       buffer.base();
  int         siglen = (int) buffer.size();
  Symbol *name;
  if (!intern_if_not_found) {
    name = SymbolTable::probe(sigstr, siglen);
  } else {
    name = SymbolTable::new_symbol(sigstr, siglen);
  }
  return name;
}

bool java_lang_invoke_MethodType::equals(oop mt1, oop mt2) {
  if (mt1 == mt2)
    return true;
  if (rtype(mt1) != rtype(mt2))
    return false;
  if (ptype_count(mt1) != ptype_count(mt2))
    return false;
  for (int i = ptype_count(mt1) - 1; i >= 0; i--) {
    if (ptype(mt1, i) != ptype(mt2, i))
      return false;
  }
  return true;
}

oop java_lang_invoke_MethodType::rtype(oop mt) {
  assert(is_instance(mt), "must be a MethodType");
  return mt->obj_field(_rtype_offset);
}

objArrayOop java_lang_invoke_MethodType::ptypes(oop mt) {
  assert(is_instance(mt), "must be a MethodType");
  return (objArrayOop) mt->obj_field(_ptypes_offset);
}

oop java_lang_invoke_MethodType::ptype(oop mt, int idx) {
  return ptypes(mt)->obj_at(idx);
}

int java_lang_invoke_MethodType::ptype_count(oop mt) {
  return ptypes(mt)->length();
}

int java_lang_invoke_MethodType::ptype_slot_count(oop mt) {
  objArrayOop pts = ptypes(mt);
  int count = pts->length();
  int slots = 0;
  for (int i = 0; i < count; i++) {
    BasicType bt = java_lang_Class::as_BasicType(pts->obj_at(i));
    slots += type2size[bt];
  }
  return slots;
}

int java_lang_invoke_MethodType::rtype_slot_count(oop mt) {
  BasicType bt = java_lang_Class::as_BasicType(rtype(mt));
  return type2size[bt];
}


// Support for java_lang_invoke_CallSite

int java_lang_invoke_CallSite::_target_offset;
int java_lang_invoke_CallSite::_context_offset;

#define CALLSITE_FIELDS_DO(macro) \
  macro(_target_offset,  k, "target", java_lang_invoke_MethodHandle_signature, false); \
  macro(_context_offset, k, "context", java_lang_invoke_MethodHandleNatives_CallSiteContext_signature, false)

void java_lang_invoke_CallSite::compute_offsets() {
  InstanceKlass* k = SystemDictionary::CallSite_klass();
  CALLSITE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_CallSite::serialize_offsets(SerializeClosure* f) {
  CALLSITE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_lang_invoke_CallSite::context_no_keepalive(oop call_site) {
  assert(java_lang_invoke_CallSite::is_instance(call_site), "");

  oop dep_oop = call_site->obj_field_access<AS_NO_KEEPALIVE>(_context_offset);
  return dep_oop;
}

// Support for java_lang_invoke_MethodHandleNatives_CallSiteContext

int java_lang_invoke_MethodHandleNatives_CallSiteContext::_vmdependencies_offset;
int java_lang_invoke_MethodHandleNatives_CallSiteContext::_last_cleanup_offset;

void java_lang_invoke_MethodHandleNatives_CallSiteContext::compute_offsets() {
  InstanceKlass* k = SystemDictionary::Context_klass();
  CALLSITECONTEXT_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_invoke_MethodHandleNatives_CallSiteContext::serialize_offsets(SerializeClosure* f) {
  CALLSITECONTEXT_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

DependencyContext java_lang_invoke_MethodHandleNatives_CallSiteContext::vmdependencies(oop call_site) {
  assert(java_lang_invoke_MethodHandleNatives_CallSiteContext::is_instance(call_site), "");
  nmethodBucket* volatile* vmdeps_addr = (nmethodBucket* volatile*)call_site->field_addr(_vmdependencies_offset);
  volatile uint64_t* last_cleanup_addr = (volatile uint64_t*)call_site->field_addr(_last_cleanup_offset);
  DependencyContext dep_ctx(vmdeps_addr, last_cleanup_addr);
  return dep_ctx;
}

// Support for java_security_AccessControlContext

int java_security_AccessControlContext::_context_offset = 0;
int java_security_AccessControlContext::_privilegedContext_offset = 0;
int java_security_AccessControlContext::_isPrivileged_offset = 0;
int java_security_AccessControlContext::_isAuthorized_offset = -1;

#define ACCESSCONTROLCONTEXT_FIELDS_DO(macro) \
  macro(_context_offset,           k, "context",      protectiondomain_signature, false); \
  macro(_privilegedContext_offset, k, "privilegedContext", accesscontrolcontext_signature, false); \
  macro(_isPrivileged_offset,      k, "isPrivileged", bool_signature, false); \
  macro(_isAuthorized_offset,      k, "isAuthorized", bool_signature, false)

void java_security_AccessControlContext::compute_offsets() {
  assert(_isPrivileged_offset == 0, "offsets should be initialized only once");
  InstanceKlass* k = SystemDictionary::AccessControlContext_klass();
  ACCESSCONTROLCONTEXT_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_security_AccessControlContext::serialize_offsets(SerializeClosure* f) {
  ACCESSCONTROLCONTEXT_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_security_AccessControlContext::create(objArrayHandle context, bool isPrivileged, Handle privileged_context, TRAPS) {
  assert(_isPrivileged_offset != 0, "offsets should have been initialized");
  // Ensure klass is initialized
  SystemDictionary::AccessControlContext_klass()->initialize(CHECK_0);
  // Allocate result
  oop result = SystemDictionary::AccessControlContext_klass()->allocate_instance(CHECK_0);
  // Fill in values
  result->obj_field_put(_context_offset, context());
  result->obj_field_put(_privilegedContext_offset, privileged_context());
  result->bool_field_put(_isPrivileged_offset, isPrivileged);
  // whitelist AccessControlContexts created by the JVM if present
  if (_isAuthorized_offset != -1) {
    result->bool_field_put(_isAuthorized_offset, true);
  }
  return result;
}


// Support for java_lang_ClassLoader

bool java_lang_ClassLoader::offsets_computed = false;
int  java_lang_ClassLoader::_loader_data_offset = -1;
int  java_lang_ClassLoader::parallelCapable_offset = -1;
int  java_lang_ClassLoader::name_offset = -1;
int  java_lang_ClassLoader::nameAndId_offset = -1;
int  java_lang_ClassLoader::unnamedModule_offset = -1;

ClassLoaderData* java_lang_ClassLoader::loader_data_acquire(oop loader) {
  assert(loader != NULL && oopDesc::is_oop(loader), "loader must be oop");
  return HeapAccess<MO_ACQUIRE>::load_at(loader, _loader_data_offset);
}

ClassLoaderData* java_lang_ClassLoader::loader_data_raw(oop loader) {
  assert(loader != NULL && oopDesc::is_oop(loader), "loader must be oop");
  return RawAccess<>::load_at(loader, _loader_data_offset);
}

void java_lang_ClassLoader::release_set_loader_data(oop loader, ClassLoaderData* new_data) {
  assert(loader != NULL && oopDesc::is_oop(loader), "loader must be oop");
  HeapAccess<MO_RELEASE>::store_at(loader, _loader_data_offset, new_data);
}

#define CLASSLOADER_FIELDS_DO(macro) \
  macro(parallelCapable_offset, k1, "parallelLockMap",      concurrenthashmap_signature, false); \
  macro(name_offset,            k1, vmSymbols::name_name(), string_signature, false); \
  macro(nameAndId_offset,       k1, "nameAndId",            string_signature, false); \
  macro(unnamedModule_offset,   k1, "unnamedModule",        module_signature, false); \
  macro(parent_offset,          k1, "parent",               classloader_signature, false)

void java_lang_ClassLoader::compute_offsets() {
  assert(!offsets_computed, "offsets should be initialized only once");
  offsets_computed = true;

  InstanceKlass* k1 = SystemDictionary::ClassLoader_klass();
  CLASSLOADER_FIELDS_DO(FIELD_COMPUTE_OFFSET);

  CLASSLOADER_INJECTED_FIELDS(INJECTED_FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_ClassLoader::serialize_offsets(SerializeClosure* f) {
  CLASSLOADER_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
  CLASSLOADER_INJECTED_FIELDS(INJECTED_FIELD_SERIALIZE_OFFSET);
}
#endif

oop java_lang_ClassLoader::parent(oop loader) {
  assert(is_instance(loader), "loader must be oop");
  return loader->obj_field(parent_offset);
}

// Returns the name field of this class loader.  If the name field has not
// been set, null will be returned.
oop java_lang_ClassLoader::name(oop loader) {
  assert(is_instance(loader), "loader must be oop");
  return loader->obj_field(name_offset);
}

// Returns the nameAndId field of this class loader. The format is
// as follows:
//   If the defining loader has a name explicitly set then '<loader-name>' @<id>
//   If the defining loader has no name then <qualified-class-name> @<id>
//   If built-in loader, then omit '@<id>' as there is only one instance.
// Use ClassLoader::loader_name_id() to obtain this String as a char*.
oop java_lang_ClassLoader::nameAndId(oop loader) {
  assert(is_instance(loader), "loader must be oop");
  return loader->obj_field(nameAndId_offset);
}

bool java_lang_ClassLoader::isAncestor(oop loader, oop cl) {
  assert(is_instance(loader), "loader must be oop");
  assert(cl == NULL || is_instance(cl), "cl argument must be oop");
  oop acl = loader;
  debug_only(jint loop_count = 0);
  // This loop taken verbatim from ClassLoader.java:
  do {
    acl = parent(acl);
    if (cl == acl) {
      return true;
    }
    assert(++loop_count > 0, "loop_count overflow");
  } while (acl != NULL);
  return false;
}

bool java_lang_ClassLoader::is_instance(oop obj) {
  return obj != NULL && is_subclass(obj->klass());
}


// For class loader classes, parallelCapable defined
// based on non-null field
// Written to by java.lang.ClassLoader, vm only reads this field, doesn't set it
bool java_lang_ClassLoader::parallelCapable(oop class_loader) {
  if (parallelCapable_offset == -1) {
     // Default for backward compatibility is false
     return false;
  }
  return (class_loader->obj_field(parallelCapable_offset) != NULL);
}

bool java_lang_ClassLoader::is_trusted_loader(oop loader) {
  // Fix for 4474172; see evaluation for more details
  loader = non_reflection_class_loader(loader);

  oop cl = SystemDictionary::java_system_loader();
  while(cl != NULL) {
    if (cl == loader) return true;
    cl = parent(cl);
  }
  return false;
}

// Return true if this is one of the class loaders associated with
// the generated bytecodes for reflection.
bool java_lang_ClassLoader::is_reflection_class_loader(oop loader) {
  if (loader != NULL) {
    Klass* delegating_cl_class = SystemDictionary::reflect_DelegatingClassLoader_klass();
    // This might be null in non-1.4 JDKs
    return (delegating_cl_class != NULL && loader->is_a(delegating_cl_class));
  }
  return false;
}

oop java_lang_ClassLoader::non_reflection_class_loader(oop loader) {
  // See whether this is one of the class loaders associated with
  // the generated bytecodes for reflection, and if so, "magically"
  // delegate to its parent to prevent class loading from occurring
  // in places where applications using reflection didn't expect it.
  if (is_reflection_class_loader(loader)) {
    return parent(loader);
  }
  return loader;
}

oop java_lang_ClassLoader::unnamedModule(oop loader) {
  assert(is_instance(loader), "loader must be oop");
  return loader->obj_field(unnamedModule_offset);
}

// Support for java_lang_System
//
#define SYSTEM_FIELDS_DO(macro) \
  macro(static_in_offset,  k, "in",  input_stream_signature, true); \
  macro(static_out_offset, k, "out", print_stream_signature, true); \
  macro(static_err_offset, k, "err", print_stream_signature, true); \
  macro(static_security_offset, k, "security", security_manager_signature, true)

void java_lang_System::compute_offsets() {
  InstanceKlass* k = SystemDictionary::System_klass();
  SYSTEM_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_System::serialize_offsets(SerializeClosure* f) {
   SYSTEM_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

int java_lang_System::in_offset_in_bytes() { return static_in_offset; }
int java_lang_System::out_offset_in_bytes() { return static_out_offset; }
int java_lang_System::err_offset_in_bytes() { return static_err_offset; }

// Support for jdk_internal_misc_UnsafeConstants
//
class UnsafeConstantsFixup : public FieldClosure {
private:
  int _address_size;
  int _page_size;
  bool _big_endian;
  bool _use_unaligned_access;
  int _data_cache_line_flush_size;
public:
  UnsafeConstantsFixup() {
    // round up values for all static final fields
    _address_size = sizeof(void*);
    _page_size = os::vm_page_size();
    _big_endian = LITTLE_ENDIAN_ONLY(false) BIG_ENDIAN_ONLY(true);
    _use_unaligned_access = UseUnalignedAccesses;
    _data_cache_line_flush_size = (int)VM_Version::data_cache_line_flush_size();
  }

  void do_field(fieldDescriptor* fd) {
    oop mirror = fd->field_holder()->java_mirror();
    assert(mirror != NULL, "UnsafeConstants must have mirror already");
    assert(fd->field_holder() == SystemDictionary::UnsafeConstants_klass(), "Should be UnsafeConstants");
    assert(fd->is_final(), "fields of UnsafeConstants must be final");
    assert(fd->is_static(), "fields of UnsafeConstants must be static");
    if (fd->name() == vmSymbols::address_size_name()) {
      mirror->int_field_put(fd->offset(), _address_size);
    } else if (fd->name() == vmSymbols::page_size_name()) {
      mirror->int_field_put(fd->offset(), _page_size);
    } else if (fd->name() == vmSymbols::big_endian_name()) {
      mirror->bool_field_put(fd->offset(), _big_endian);
    } else if (fd->name() == vmSymbols::use_unaligned_access_name()) {
      mirror->bool_field_put(fd->offset(), _use_unaligned_access);
    } else if (fd->name() == vmSymbols::data_cache_line_flush_size_name()) {
      mirror->int_field_put(fd->offset(), _data_cache_line_flush_size);
    } else {
      assert(false, "unexpected UnsafeConstants field");
    }
  }
};

void jdk_internal_misc_UnsafeConstants::set_unsafe_constants() {
  UnsafeConstantsFixup fixup;
  SystemDictionary::UnsafeConstants_klass()->do_local_static_fields(&fixup);
}

int java_lang_Class::_klass_offset;
int java_lang_Class::_array_klass_offset;
int java_lang_Class::_oop_size_offset;
int java_lang_Class::_static_oop_field_count_offset;
int java_lang_Class::_class_loader_offset;
int java_lang_Class::_module_offset;
int java_lang_Class::_protection_domain_offset;
int java_lang_Class::_component_mirror_offset;
int java_lang_Class::_init_lock_offset;
int java_lang_Class::_signers_offset;
int java_lang_Class::_name_offset;
int java_lang_Class::_source_file_offset;
GrowableArray<Klass*>* java_lang_Class::_fixup_mirror_list = NULL;
GrowableArray<Klass*>* java_lang_Class::_fixup_module_field_list = NULL;
int java_lang_Throwable::backtrace_offset;
int java_lang_Throwable::detailMessage_offset;
int java_lang_Throwable::stackTrace_offset;
int java_lang_Throwable::depth_offset;
int java_lang_Throwable::static_unassigned_stacktrace_offset;
int java_lang_reflect_AccessibleObject::override_offset;
int java_lang_reflect_Method::clazz_offset;
int java_lang_reflect_Method::name_offset;
int java_lang_reflect_Method::returnType_offset;
int java_lang_reflect_Method::parameterTypes_offset;
int java_lang_reflect_Method::exceptionTypes_offset;
int java_lang_reflect_Method::slot_offset;
int java_lang_reflect_Method::modifiers_offset;
int java_lang_reflect_Method::signature_offset;
int java_lang_reflect_Method::annotations_offset;
int java_lang_reflect_Method::parameter_annotations_offset;
int java_lang_reflect_Method::annotation_default_offset;
int java_lang_reflect_Constructor::clazz_offset;
int java_lang_reflect_Constructor::parameterTypes_offset;
int java_lang_reflect_Constructor::exceptionTypes_offset;
int java_lang_reflect_Constructor::slot_offset;
int java_lang_reflect_Constructor::modifiers_offset;
int java_lang_reflect_Constructor::signature_offset;
int java_lang_reflect_Constructor::annotations_offset;
int java_lang_reflect_Constructor::parameter_annotations_offset;
int java_lang_reflect_Field::clazz_offset;
int java_lang_reflect_Field::name_offset;
int java_lang_reflect_Field::type_offset;
int java_lang_reflect_Field::slot_offset;
int java_lang_reflect_Field::modifiers_offset;
int java_lang_reflect_Field::signature_offset;
int java_lang_reflect_Field::annotations_offset;
int java_lang_reflect_Parameter::name_offset;
int java_lang_reflect_Parameter::modifiers_offset;
int java_lang_reflect_Parameter::index_offset;
int java_lang_reflect_Parameter::executable_offset;
int java_lang_boxing_object::value_offset;
int java_lang_boxing_object::long_value_offset;
int java_lang_ref_Reference::referent_offset;
int java_lang_ref_Reference::queue_offset;
int java_lang_ref_Reference::next_offset;
int java_lang_ref_Reference::discovered_offset;
int java_lang_ref_SoftReference::timestamp_offset;
int java_lang_ref_SoftReference::static_clock_offset;
int java_lang_ClassLoader::parent_offset;
int java_lang_System::static_in_offset;
int java_lang_System::static_out_offset;
int java_lang_System::static_err_offset;
int java_lang_System::static_security_offset;
int java_lang_StackTraceElement::methodName_offset;
int java_lang_StackTraceElement::fileName_offset;
int java_lang_StackTraceElement::lineNumber_offset;
int java_lang_StackTraceElement::moduleName_offset;
int java_lang_StackTraceElement::moduleVersion_offset;
int java_lang_StackTraceElement::classLoaderName_offset;
int java_lang_StackTraceElement::declaringClass_offset;
int java_lang_StackTraceElement::declaringClassObject_offset;
int java_lang_StackFrameInfo::_memberName_offset;
int java_lang_StackFrameInfo::_bci_offset;
int java_lang_StackFrameInfo::_version_offset;
int java_lang_LiveStackFrameInfo::_monitors_offset;
int java_lang_LiveStackFrameInfo::_locals_offset;
int java_lang_LiveStackFrameInfo::_operands_offset;
int java_lang_LiveStackFrameInfo::_mode_offset;
int java_lang_AssertionStatusDirectives::classes_offset;
int java_lang_AssertionStatusDirectives::classEnabled_offset;
int java_lang_AssertionStatusDirectives::packages_offset;
int java_lang_AssertionStatusDirectives::packageEnabled_offset;
int java_lang_AssertionStatusDirectives::deflt_offset;
int java_nio_Buffer::_limit_offset;
int java_util_concurrent_locks_AbstractOwnableSynchronizer::_owner_offset;
int reflect_ConstantPool::_oop_offset;
int reflect_UnsafeStaticFieldAccessorImpl::_base_offset;
int java_lang_Integer_IntegerCache::_static_cache_offset;
int java_lang_Long_LongCache::_static_cache_offset;
int java_lang_Character_CharacterCache::_static_cache_offset;
int java_lang_Short_ShortCache::_static_cache_offset;
int java_lang_Byte_ByteCache::_static_cache_offset;
int java_lang_Boolean::_static_TRUE_offset;
int java_lang_Boolean::_static_FALSE_offset;
int java_lang_reflect_RecordComponent::clazz_offset;
int java_lang_reflect_RecordComponent::name_offset;
int java_lang_reflect_RecordComponent::type_offset;
int java_lang_reflect_RecordComponent::accessor_offset;
int java_lang_reflect_RecordComponent::signature_offset;
int java_lang_reflect_RecordComponent::annotations_offset;
int java_lang_reflect_RecordComponent::typeAnnotations_offset;



#define STACKTRACEELEMENT_FIELDS_DO(macro) \
  macro(declaringClassObject_offset,  k, "declaringClassObject", class_signature, false); \
  macro(classLoaderName_offset, k, "classLoaderName", string_signature, false); \
  macro(moduleName_offset,      k, "moduleName",      string_signature, false); \
  macro(moduleVersion_offset,   k, "moduleVersion",   string_signature, false); \
  macro(declaringClass_offset,  k, "declaringClass",  string_signature, false); \
  macro(methodName_offset,      k, "methodName",      string_signature, false); \
  macro(fileName_offset,        k, "fileName",        string_signature, false); \
  macro(lineNumber_offset,      k, "lineNumber",      int_signature,    false)

// Support for java_lang_StackTraceElement
void java_lang_StackTraceElement::compute_offsets() {
  InstanceKlass* k = SystemDictionary::StackTraceElement_klass();
  STACKTRACEELEMENT_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_StackTraceElement::serialize_offsets(SerializeClosure* f) {
  STACKTRACEELEMENT_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

void java_lang_StackTraceElement::set_fileName(oop element, oop value) {
  element->obj_field_put(fileName_offset, value);
}

void java_lang_StackTraceElement::set_declaringClass(oop element, oop value) {
  element->obj_field_put(declaringClass_offset, value);
}

void java_lang_StackTraceElement::set_methodName(oop element, oop value) {
  element->obj_field_put(methodName_offset, value);
}

void java_lang_StackTraceElement::set_lineNumber(oop element, int value) {
  element->int_field_put(lineNumber_offset, value);
}

void java_lang_StackTraceElement::set_moduleName(oop element, oop value) {
  element->obj_field_put(moduleName_offset, value);
}

void java_lang_StackTraceElement::set_moduleVersion(oop element, oop value) {
  element->obj_field_put(moduleVersion_offset, value);
}

void java_lang_StackTraceElement::set_classLoaderName(oop element, oop value) {
  element->obj_field_put(classLoaderName_offset, value);
}

void java_lang_StackTraceElement::set_declaringClassObject(oop element, oop value) {
  element->obj_field_put(declaringClassObject_offset, value);
}

void java_lang_StackFrameInfo::set_version(oop element, short value) {
  element->short_field_put(_version_offset, value);
}

void java_lang_StackFrameInfo::set_bci(oop element, int value) {
  assert(value >= 0 && value < max_jushort, "must be a valid bci value");
  element->int_field_put(_bci_offset, value);
}

void java_lang_LiveStackFrameInfo::set_monitors(oop element, oop value) {
  element->obj_field_put(_monitors_offset, value);
}

void java_lang_LiveStackFrameInfo::set_locals(oop element, oop value) {
  element->obj_field_put(_locals_offset, value);
}

void java_lang_LiveStackFrameInfo::set_operands(oop element, oop value) {
  element->obj_field_put(_operands_offset, value);
}

void java_lang_LiveStackFrameInfo::set_mode(oop element, int value) {
  element->int_field_put(_mode_offset, value);
}

// Support for java Assertions - java_lang_AssertionStatusDirectives.
#define ASSERTIONSTATUSDIRECTIVES_FIELDS_DO(macro) \
  macro(classes_offset,        k, "classes",        string_array_signature, false); \
  macro(classEnabled_offset,   k, "classEnabled",   bool_array_signature, false); \
  macro(packages_offset,       k, "packages",       string_array_signature, false); \
  macro(packageEnabled_offset, k, "packageEnabled", bool_array_signature,   false); \
  macro(deflt_offset,          k, "deflt",          bool_signature,         false)

void java_lang_AssertionStatusDirectives::compute_offsets() {
  InstanceKlass* k = SystemDictionary::AssertionStatusDirectives_klass();
  ASSERTIONSTATUSDIRECTIVES_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_AssertionStatusDirectives::serialize_offsets(SerializeClosure* f) {
  ASSERTIONSTATUSDIRECTIVES_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

void java_lang_AssertionStatusDirectives::set_classes(oop o, oop val) {
  o->obj_field_put(classes_offset, val);
}

void java_lang_AssertionStatusDirectives::set_classEnabled(oop o, oop val) {
  o->obj_field_put(classEnabled_offset, val);
}

void java_lang_AssertionStatusDirectives::set_packages(oop o, oop val) {
  o->obj_field_put(packages_offset, val);
}

void java_lang_AssertionStatusDirectives::set_packageEnabled(oop o, oop val) {
  o->obj_field_put(packageEnabled_offset, val);
}

void java_lang_AssertionStatusDirectives::set_deflt(oop o, bool val) {
  o->bool_field_put(deflt_offset, val);
}


// Support for intrinsification of java.nio.Buffer.checkIndex
int java_nio_Buffer::limit_offset() {
  return _limit_offset;
}

#define BUFFER_FIELDS_DO(macro) \
  macro(_limit_offset, k, "limit", int_signature, false)

void java_nio_Buffer::compute_offsets() {
  InstanceKlass* k = SystemDictionary::nio_Buffer_klass();
  assert(k != NULL, "must be loaded in 1.4+");
  BUFFER_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_nio_Buffer::serialize_offsets(SerializeClosure* f) {
  BUFFER_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define AOS_FIELDS_DO(macro) \
  macro(_owner_offset, k, "exclusiveOwnerThread", thread_signature, false)

void java_util_concurrent_locks_AbstractOwnableSynchronizer::compute_offsets() {
  InstanceKlass* k = SystemDictionary::java_util_concurrent_locks_AbstractOwnableSynchronizer_klass();
  AOS_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

oop java_util_concurrent_locks_AbstractOwnableSynchronizer::get_owner_threadObj(oop obj) {
  assert(_owner_offset != 0, "Must be initialized");
  return obj->obj_field(_owner_offset);
}

#if INCLUDE_CDS
void java_util_concurrent_locks_AbstractOwnableSynchronizer::serialize_offsets(SerializeClosure* f) {
  AOS_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

#define INTEGER_CACHE_FIELDS_DO(macro) \
  macro(_static_cache_offset, k, "cache", java_lang_Integer_array_signature, true)

void java_lang_Integer_IntegerCache::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  INTEGER_CACHE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

objArrayOop java_lang_Integer_IntegerCache::cache(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return objArrayOop(base->obj_field(_static_cache_offset));
}

Symbol* java_lang_Integer_IntegerCache::symbol() {
  return vmSymbols::java_lang_Integer_IntegerCache();
}

#if INCLUDE_CDS
void java_lang_Integer_IntegerCache::serialize_offsets(SerializeClosure* f) {
  INTEGER_CACHE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef INTEGER_CACHE_FIELDS_DO

jint java_lang_Integer::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.i;
}

#define LONG_CACHE_FIELDS_DO(macro) \
  macro(_static_cache_offset, k, "cache", java_lang_Long_array_signature, true)

void java_lang_Long_LongCache::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  LONG_CACHE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

objArrayOop java_lang_Long_LongCache::cache(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return objArrayOop(base->obj_field(_static_cache_offset));
}

Symbol* java_lang_Long_LongCache::symbol() {
  return vmSymbols::java_lang_Long_LongCache();
}

#if INCLUDE_CDS
void java_lang_Long_LongCache::serialize_offsets(SerializeClosure* f) {
  LONG_CACHE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef LONG_CACHE_FIELDS_DO

jlong java_lang_Long::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.j;
}

#define CHARACTER_CACHE_FIELDS_DO(macro) \
  macro(_static_cache_offset, k, "cache", java_lang_Character_array_signature, true)

void java_lang_Character_CharacterCache::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  CHARACTER_CACHE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

objArrayOop java_lang_Character_CharacterCache::cache(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return objArrayOop(base->obj_field(_static_cache_offset));
}

Symbol* java_lang_Character_CharacterCache::symbol() {
  return vmSymbols::java_lang_Character_CharacterCache();
}

#if INCLUDE_CDS
void java_lang_Character_CharacterCache::serialize_offsets(SerializeClosure* f) {
  CHARACTER_CACHE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef CHARACTER_CACHE_FIELDS_DO

jchar java_lang_Character::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.c;
}

#define SHORT_CACHE_FIELDS_DO(macro) \
  macro(_static_cache_offset, k, "cache", java_lang_Short_array_signature, true)

void java_lang_Short_ShortCache::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  SHORT_CACHE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

objArrayOop java_lang_Short_ShortCache::cache(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return objArrayOop(base->obj_field(_static_cache_offset));
}

Symbol* java_lang_Short_ShortCache::symbol() {
  return vmSymbols::java_lang_Short_ShortCache();
}

#if INCLUDE_CDS
void java_lang_Short_ShortCache::serialize_offsets(SerializeClosure* f) {
  SHORT_CACHE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef SHORT_CACHE_FIELDS_DO

jshort java_lang_Short::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.s;
}

#define BYTE_CACHE_FIELDS_DO(macro) \
  macro(_static_cache_offset, k, "cache", java_lang_Byte_array_signature, true)

void java_lang_Byte_ByteCache::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  BYTE_CACHE_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

objArrayOop java_lang_Byte_ByteCache::cache(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return objArrayOop(base->obj_field(_static_cache_offset));
}

Symbol* java_lang_Byte_ByteCache::symbol() {
  return vmSymbols::java_lang_Byte_ByteCache();
}

#if INCLUDE_CDS
void java_lang_Byte_ByteCache::serialize_offsets(SerializeClosure* f) {
  BYTE_CACHE_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef BYTE_CACHE_FIELDS_DO

jbyte java_lang_Byte::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.b;
}
#define BOOLEAN_FIELDS_DO(macro) \
  macro(_static_TRUE_offset, k, "TRUE", java_lang_Boolean_signature, true); \
  macro(_static_FALSE_offset, k, "FALSE", java_lang_Boolean_signature, true)


void java_lang_Boolean::compute_offsets(InstanceKlass *k) {
  guarantee(k != NULL && k->is_initialized(), "must be loaded and initialized");
  BOOLEAN_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

oop java_lang_Boolean::get_TRUE(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return base->obj_field(_static_TRUE_offset);
}

oop java_lang_Boolean::get_FALSE(InstanceKlass *ik) {
  oop base = ik->static_field_base_raw();
  return base->obj_field(_static_FALSE_offset);
}

Symbol* java_lang_Boolean::symbol() {
  return vmSymbols::java_lang_Boolean();
}

#if INCLUDE_CDS
void java_lang_Boolean::serialize_offsets(SerializeClosure* f) {
  BOOLEAN_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif
#undef BOOLEAN_CACHE_FIELDS_DO

jboolean java_lang_Boolean::value(oop obj) {
   jvalue v;
   java_lang_boxing_object::get_value(obj, &v);
   return v.z;
}

static int member_offset(int hardcoded_offset) {
  return (hardcoded_offset * heapOopSize) + instanceOopDesc::base_offset_in_bytes();
}

#define RECORDCOMPONENT_FIELDS_DO(macro) \
  macro(clazz_offset,       k, "clazz",       class_signature,  false); \
  macro(name_offset,        k, "name",        string_signature, false); \
  macro(type_offset,        k, "type",        class_signature,  false); \
  macro(accessor_offset,    k, "accessor",    reflect_method_signature, false); \
  macro(signature_offset,   k, "signature",   string_signature, false); \
  macro(annotations_offset, k, "annotations", byte_array_signature,     false); \
  macro(typeAnnotations_offset, k, "typeAnnotations", byte_array_signature, false);

// Support for java_lang_reflect_RecordComponent
void java_lang_reflect_RecordComponent::compute_offsets() {
  InstanceKlass* k = SystemDictionary::RecordComponent_klass();
  RECORDCOMPONENT_FIELDS_DO(FIELD_COMPUTE_OFFSET);
}

#if INCLUDE_CDS
void java_lang_reflect_RecordComponent::serialize_offsets(SerializeClosure* f) {
  RECORDCOMPONENT_FIELDS_DO(FIELD_SERIALIZE_OFFSET);
}
#endif

void java_lang_reflect_RecordComponent::set_clazz(oop element, oop value) {
  element->obj_field_put(clazz_offset, value);
}

void java_lang_reflect_RecordComponent::set_name(oop element, oop value) {
  element->obj_field_put(name_offset, value);
}

void java_lang_reflect_RecordComponent::set_type(oop element, oop value) {
  element->obj_field_put(type_offset, value);
}

void java_lang_reflect_RecordComponent::set_accessor(oop element, oop value) {
  element->obj_field_put(accessor_offset, value);
}

void java_lang_reflect_RecordComponent::set_signature(oop element, oop value) {
  element->obj_field_put(signature_offset, value);
}

void java_lang_reflect_RecordComponent::set_annotations(oop element, oop value) {
  element->obj_field_put(annotations_offset, value);
}

void java_lang_reflect_RecordComponent::set_typeAnnotations(oop element, oop value) {
  element->obj_field_put(typeAnnotations_offset, value);
}

// Compute hard-coded offsets
// Invoked before SystemDictionary::initialize, so pre-loaded classes
// are not available to determine the offset_of_static_fields.
void JavaClasses::compute_hard_coded_offsets() {

  // java_lang_boxing_object
  java_lang_boxing_object::value_offset      = member_offset(java_lang_boxing_object::hc_value_offset);
  java_lang_boxing_object::long_value_offset = align_up(member_offset(java_lang_boxing_object::hc_value_offset), BytesPerLong);

  // java_lang_ref_Reference
  java_lang_ref_Reference::referent_offset    = member_offset(java_lang_ref_Reference::hc_referent_offset);
  java_lang_ref_Reference::queue_offset       = member_offset(java_lang_ref_Reference::hc_queue_offset);
  java_lang_ref_Reference::next_offset        = member_offset(java_lang_ref_Reference::hc_next_offset);
  java_lang_ref_Reference::discovered_offset  = member_offset(java_lang_ref_Reference::hc_discovered_offset);
}

#define DO_COMPUTE_OFFSETS(k) k::compute_offsets();

// Compute non-hard-coded field offsets of all the classes in this file
void JavaClasses::compute_offsets() {
  if (UseSharedSpaces) {
    assert(JvmtiExport::is_early_phase() && !(JvmtiExport::should_post_class_file_load_hook() &&
                                              JvmtiExport::has_early_class_hook_env()),
           "JavaClasses::compute_offsets() must be called in early JVMTI phase.");
    // None of the classes used by the rest of this function can be replaced by
    // JMVTI ClassFileLoadHook.
    // We are safe to use the archived offsets, which have already been restored
    // by JavaClasses::serialize_offsets, without computing the offsets again.
    return;
  }

  // We have already called the compute_offsets() of the
  // BASIC_JAVA_CLASSES_DO_PART1 classes (java_lang_String and java_lang_Class)
  // earlier inside SystemDictionary::resolve_well_known_classes()
  BASIC_JAVA_CLASSES_DO_PART2(DO_COMPUTE_OFFSETS);
}

#if INCLUDE_CDS
#define DO_SERIALIZE_OFFSETS(k) k::serialize_offsets(soc);

void JavaClasses::serialize_offsets(SerializeClosure* soc) {
  BASIC_JAVA_CLASSES_DO(DO_SERIALIZE_OFFSETS);
}
#endif


#ifndef PRODUCT

// These functions exist to assert the validity of hard-coded field offsets to guard
// against changes in the class files

bool JavaClasses::check_offset(const char *klass_name, int hardcoded_offset, const char *field_name, const char* field_sig) {
  EXCEPTION_MARK;
  fieldDescriptor fd;
  TempNewSymbol klass_sym = SymbolTable::new_symbol(klass_name);
  Klass* k = SystemDictionary::resolve_or_fail(klass_sym, true, CATCH);
  InstanceKlass* ik = InstanceKlass::cast(k);
  TempNewSymbol f_name = SymbolTable::new_symbol(field_name);
  TempNewSymbol f_sig  = SymbolTable::new_symbol(field_sig);
  if (!ik->find_local_field(f_name, f_sig, &fd)) {
    tty->print_cr("Nonstatic field %s.%s not found", klass_name, field_name);
    return false;
  }
  if (fd.is_static()) {
    tty->print_cr("Nonstatic field %s.%s appears to be static", klass_name, field_name);
    return false;
  }
  if (fd.offset() == hardcoded_offset ) {
    return true;
  } else {
    tty->print_cr("Offset of nonstatic field %s.%s is hardcoded as %d but should really be %d.",
                  klass_name, field_name, hardcoded_offset, fd.offset());
    return false;
  }
}

// Check the hard-coded field offsets of all the classes in this file

void JavaClasses::check_offsets() {
  bool valid = true;

#define CHECK_OFFSET(klass_name, cpp_klass_name, field_name, field_sig) \
  valid &= check_offset(klass_name, cpp_klass_name :: field_name ## _offset, #field_name, field_sig)

#define CHECK_LONG_OFFSET(klass_name, cpp_klass_name, field_name, field_sig) \
  valid &= check_offset(klass_name, cpp_klass_name :: long_ ## field_name ## _offset, #field_name, field_sig)

  // Boxed primitive objects (java_lang_boxing_object)

  CHECK_OFFSET("java/lang/Boolean",   java_lang_boxing_object, value, "Z");
  CHECK_OFFSET("java/lang/Character", java_lang_boxing_object, value, "C");
  CHECK_OFFSET("java/lang/Float",     java_lang_boxing_object, value, "F");
  CHECK_LONG_OFFSET("java/lang/Double", java_lang_boxing_object, value, "D");
  CHECK_OFFSET("java/lang/Byte",      java_lang_boxing_object, value, "B");
  CHECK_OFFSET("java/lang/Short",     java_lang_boxing_object, value, "S");
  CHECK_OFFSET("java/lang/Integer",   java_lang_boxing_object, value, "I");
  CHECK_LONG_OFFSET("java/lang/Long", java_lang_boxing_object, value, "J");

  // java.lang.ref.Reference

  CHECK_OFFSET("java/lang/ref/Reference", java_lang_ref_Reference, referent, "Ljava/lang/Object;");
  CHECK_OFFSET("java/lang/ref/Reference", java_lang_ref_Reference, queue, "Ljava/lang/ref/ReferenceQueue;");
  CHECK_OFFSET("java/lang/ref/Reference", java_lang_ref_Reference, next, "Ljava/lang/ref/Reference;");
  // Fake field
  //CHECK_OFFSET("java/lang/ref/Reference", java_lang_ref_Reference, discovered, "Ljava/lang/ref/Reference;");

  if (!valid) vm_exit_during_initialization("Hard-coded field offset verification failed");
}

#endif // PRODUCT

int InjectedField::compute_offset() {
  InstanceKlass* ik = InstanceKlass::cast(klass());
  for (AllFieldStream fs(ik); !fs.done(); fs.next()) {
    if (!may_be_java && !fs.access_flags().is_internal()) {
      // Only look at injected fields
      continue;
    }
    if (fs.name() == name() && fs.signature() == signature()) {
      return fs.offset();
    }
  }
  ResourceMark rm;
  tty->print_cr("Invalid layout of %s at %s/%s%s", ik->external_name(), name()->as_C_string(), signature()->as_C_string(), may_be_java ? " (may_be_java)" : "");
#ifndef PRODUCT
  ik->print();
  tty->print_cr("all fields:");
  for (AllFieldStream fs(ik); !fs.done(); fs.next()) {
    tty->print_cr("  name: %s, sig: %s, flags: %08x", fs.name()->as_C_string(), fs.signature()->as_C_string(), fs.access_flags().as_int());
  }
#endif //PRODUCT
  vm_exit_during_initialization("Invalid layout of well-known class: use -Xlog:class+load=info to see the origin of the problem class");
  return -1;
}

void javaClasses_init() {
  JavaClasses::compute_offsets();
  JavaClasses::check_offsets();
  FilteredFieldsMap::initialize();  // must be done after computing offsets.
}
