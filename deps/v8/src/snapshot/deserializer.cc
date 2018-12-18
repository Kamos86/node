// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/deserializer.h"

#include "src/assembler-inl.h"
#include "src/heap/heap-write-barrier-inl.h"
#include "src/interpreter/interpreter.h"
#include "src/isolate.h"
#include "src/log.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/cell-inl.h"
#include "src/objects/hash-table.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/maybe-object.h"
#include "src/objects/slots.h"
#include "src/objects/smi.h"
#include "src/objects/string.h"
#include "src/snapshot/natives.h"
#include "src/snapshot/snapshot.h"

namespace v8 {
namespace internal {

// This is like a MaybeObjectSlot, except it doesn't enforce alignment.
// Most slots used below are aligned, but when writing into Code objects,
// they might not be, hence the use of UnalignedSlot and UnalignedCopy.
class UnalignedSlot {
 public:
  explicit UnalignedSlot(ObjectSlot slot) : ptr_(slot.address()) {}
  explicit UnalignedSlot(Address address) : ptr_(address) {}
  explicit UnalignedSlot(MaybeObject* slot)
      : ptr_(reinterpret_cast<Address>(slot)) {}
  explicit UnalignedSlot(Object** slot)
      : ptr_(reinterpret_cast<Address>(slot)) {}

  inline bool operator<(const UnalignedSlot& other) const {
    return ptr_ < other.ptr_;
  }
  inline bool operator==(const UnalignedSlot& other) const {
    return ptr_ == other.ptr_;
  }

  inline void Advance(int bytes = kPointerSize) { ptr_ += bytes; }

  MaybeObject Read() {
    Address result;
    memcpy(&result, reinterpret_cast<void*>(ptr_), sizeof(result));
    return MaybeObject(result);
  }
  MaybeObject ReadPrevious() {
    Address result;
    memcpy(&result, reinterpret_cast<void*>(ptr_ - kPointerSize),
           sizeof(result));
    return MaybeObject(result);
  }
  inline void Write(Address value) {
    memcpy(reinterpret_cast<void*>(ptr_), &value, sizeof(value));
  }
  MaybeObjectSlot Slot() { return MaybeObjectSlot(ptr_); }

  Address address() { return ptr_; }

 private:
  Address ptr_;
};

void Deserializer::UnalignedCopy(UnalignedSlot dest, MaybeObject value) {
  DCHECK(!allocator()->next_reference_is_weak());
  dest.Write(value.ptr());
}

void Deserializer::UnalignedCopy(UnalignedSlot dest, Address value) {
  DCHECK(!allocator()->next_reference_is_weak());
  dest.Write(value);
}

void Deserializer::Initialize(Isolate* isolate) {
  DCHECK_NULL(isolate_);
  DCHECK_NOT_NULL(isolate);
  isolate_ = isolate;
  DCHECK_NULL(external_reference_table_);
  external_reference_table_ = isolate->external_reference_table();
#ifdef DEBUG
  // Count the number of external references registered through the API.
  num_api_references_ = 0;
  if (isolate_->api_external_references() != nullptr) {
    while (isolate_->api_external_references()[num_api_references_] != 0) {
      num_api_references_++;
    }
  }
#endif  // DEBUG
  CHECK_EQ(magic_number_, SerializedData::kMagicNumber);
}

void Deserializer::Rehash() {
  DCHECK(can_rehash() || deserializing_user_code());
  for (const auto& item : to_rehash_) item->RehashBasedOnMap(isolate());
}

Deserializer::~Deserializer() {
#ifdef DEBUG
  // Do not perform checks if we aborted deserialization.
  if (source_.position() == 0) return;
  // Check that we only have padding bytes remaining.
  while (source_.HasMore()) DCHECK_EQ(kNop, source_.Get());
  // Check that we've fully used all reserved space.
  DCHECK(allocator()->ReservationsAreFullyUsed());
#endif  // DEBUG
}

// This is called on the roots.  It is the driver of the deserialization
// process.  It is also called on the body of each function.
void Deserializer::VisitRootPointers(Root root, const char* description,
                                     FullObjectSlot start, FullObjectSlot end) {
  // The space must be new space.  Any other space would cause ReadChunk to try
  // to update the remembered using nullptr as the address.
  // TODO(ishell): this will not work once we actually compress pointers.
  STATIC_ASSERT(kTaggedSize == kSystemPointerSize);
  ReadData(UnalignedSlot(start.address()), UnalignedSlot(end.address()),
           NEW_SPACE, kNullAddress);
}

void Deserializer::Synchronize(VisitorSynchronization::SyncTag tag) {
  static const byte expected = kSynchronize;
  CHECK_EQ(expected, source_.Get());
}

void Deserializer::DeserializeDeferredObjects() {
  for (int code = source_.Get(); code != kSynchronize; code = source_.Get()) {
    switch (code) {
      case kAlignmentPrefix:
      case kAlignmentPrefix + 1:
      case kAlignmentPrefix + 2: {
        int alignment = code - (SerializerDeserializer::kAlignmentPrefix - 1);
        allocator()->SetAlignment(static_cast<AllocationAlignment>(alignment));
        break;
      }
      default: {
        int space = code & kSpaceMask;
        DCHECK_LE(space, kNumberOfSpaces);
        DCHECK_EQ(code - space, kNewObject);
        HeapObject* object = GetBackReferencedObject(space);
        int size = source_.GetInt() << kPointerSizeLog2;
        Address obj_address = object->address();
        UnalignedSlot start(obj_address + kPointerSize);
        UnalignedSlot end(obj_address + size);
        bool filled = ReadData(start, end, space, obj_address);
        CHECK(filled);
        DCHECK(CanBeDeferred(object));
        PostProcessNewObject(object, space);
      }
    }
  }
}

void Deserializer::LogNewObjectEvents() {
  {
    // {new_maps_} and {new_code_objects_} are vectors containing raw
    // pointers, hence there should be no GC happening.
    DisallowHeapAllocation no_gc;
    // Issue code events for newly deserialized code objects.
    LOG_CODE_EVENT(isolate_, LogCodeObjects());
  }
  LOG_CODE_EVENT(isolate_, LogCompiledFunctions());
  LogNewMapEvents();
}

void Deserializer::LogNewMapEvents() {
  DisallowHeapAllocation no_gc;
  for (Map map : new_maps()) {
    DCHECK(FLAG_trace_maps);
    LOG(isolate_, MapCreate(map));
    LOG(isolate_, MapDetails(map));
  }
}

void Deserializer::LogScriptEvents(Script script) {
  DisallowHeapAllocation no_gc;
  LOG(isolate_,
      ScriptEvent(Logger::ScriptEventType::kDeserialize, script->id()));
  LOG(isolate_, ScriptDetails(script));
}

StringTableInsertionKey::StringTableInsertionKey(String string)
    : StringTableKey(ComputeHashField(string)), string_(string) {
  DCHECK(string->IsInternalizedString());
}

bool StringTableInsertionKey::IsMatch(Object* string) {
  // We know that all entries in a hash table had their hash keys created.
  // Use that knowledge to have fast failure.
  if (Hash() != String::cast(string)->Hash()) return false;
  // We want to compare the content of two internalized strings here.
  return string_->SlowEquals(String::cast(string));
}

Handle<String> StringTableInsertionKey::AsHandle(Isolate* isolate) {
  return handle(string_, isolate);
}

uint32_t StringTableInsertionKey::ComputeHashField(String string) {
  // Make sure hash_field() is computed.
  string->Hash();
  return string->hash_field();
}

HeapObject* Deserializer::PostProcessNewObject(HeapObject* obj, int space) {
  if ((FLAG_rehash_snapshot && can_rehash_) || deserializing_user_code()) {
    if (obj->IsString()) {
      // Uninitialize hash field as we need to recompute the hash.
      String string = String::cast(obj);
      string->set_hash_field(String::kEmptyHashField);
    } else if (obj->NeedsRehashing()) {
      to_rehash_.push_back(obj);
    }
  }

  if (deserializing_user_code()) {
    if (obj->IsString()) {
      String string = String::cast(obj);
      if (string->IsInternalizedString()) {
        // Canonicalize the internalized string. If it already exists in the
        // string table, set it to forward to the existing one.
        StringTableInsertionKey key(string);
        String canonical =
            StringTable::ForwardStringIfExists(isolate_, &key, string);

        if (!canonical.is_null()) return canonical;

        new_internalized_strings_.push_back(handle(string, isolate_));
        return string;
      }
    } else if (obj->IsScript()) {
      new_scripts_.push_back(handle(Script::cast(obj), isolate_));
    } else if (obj->IsAllocationSite()) {
      // We should link new allocation sites, but we can't do this immediately
      // because |AllocationSite::HasWeakNext()| internally accesses
      // |Heap::roots_| that may not have been initialized yet. So defer this to
      // |ObjectDeserializer::CommitPostProcessedObjects()|.
      new_allocation_sites_.push_back(AllocationSite::cast(obj));
    } else {
      DCHECK(CanBeDeferred(obj));
    }
  }
  if (obj->IsScript()) {
    LogScriptEvents(Script::cast(obj));
  } else if (obj->IsCode()) {
    // We flush all code pages after deserializing the startup snapshot.
    // Hence we only remember each individual code object when deserializing
    // user code.
    if (deserializing_user_code() || space == LO_SPACE) {
      new_code_objects_.push_back(Code::cast(obj));
    }
  } else if (FLAG_trace_maps && obj->IsMap()) {
    // Keep track of all seen Maps to log them later since they might be only
    // partially initialized at this point.
    new_maps_.push_back(Map::cast(obj));
  } else if (obj->IsAccessorInfo()) {
#ifdef USE_SIMULATOR
    accessor_infos_.push_back(AccessorInfo::cast(obj));
#endif
  } else if (obj->IsCallHandlerInfo()) {
#ifdef USE_SIMULATOR
    call_handler_infos_.push_back(CallHandlerInfo::cast(obj));
#endif
  } else if (obj->IsExternalString()) {
    if (obj->map() == ReadOnlyRoots(isolate_).native_source_string_map()) {
      ExternalOneByteString string = ExternalOneByteString::cast(obj);
      DCHECK(string->is_uncached());
      string->SetResource(
          isolate_, NativesExternalStringResource::DecodeForDeserialization(
                        string->resource()));
    } else {
      ExternalString string = ExternalString::cast(obj);
      uint32_t index = string->resource_as_uint32();
      Address address =
          static_cast<Address>(isolate_->api_external_references()[index]);
      string->set_address_as_resource(address);
      isolate_->heap()->UpdateExternalString(string, 0,
                                             string->ExternalPayloadSize());
    }
    isolate_->heap()->RegisterExternalString(String::cast(obj));
  } else if (obj->IsJSTypedArray()) {
    JSTypedArray typed_array = JSTypedArray::cast(obj);
    CHECK_LE(typed_array->byte_offset(), Smi::kMaxValue);
    int32_t byte_offset = static_cast<int32_t>(typed_array->byte_offset());
    if (byte_offset > 0) {
      FixedTypedArrayBase elements =
          FixedTypedArrayBase::cast(typed_array->elements());
      // Must be off-heap layout.
      DCHECK(!typed_array->is_on_heap());

      void* pointer_with_offset = reinterpret_cast<void*>(
          reinterpret_cast<intptr_t>(elements->external_pointer()) +
          byte_offset);
      elements->set_external_pointer(pointer_with_offset);
    }
  } else if (obj->IsJSArrayBuffer()) {
    JSArrayBuffer buffer = JSArrayBuffer::cast(obj);
    // Only fixup for the off-heap case.
    if (buffer->backing_store() != nullptr) {
      Smi store_index(reinterpret_cast<Address>(buffer->backing_store()));
      void* backing_store = off_heap_backing_stores_[store_index->value()];

      buffer->set_backing_store(backing_store);
      isolate_->heap()->RegisterNewArrayBuffer(buffer);
    }
  } else if (obj->IsFixedTypedArrayBase()) {
    FixedTypedArrayBase fta = FixedTypedArrayBase::cast(obj);
    // Only fixup for the off-heap case.
    if (fta->base_pointer() == Smi::kZero) {
      Smi store_index(reinterpret_cast<Address>(fta->external_pointer()));
      void* backing_store = off_heap_backing_stores_[store_index->value()];
      fta->set_external_pointer(backing_store);
    }
  } else if (obj->IsBytecodeArray()) {
    // TODO(mythria): Remove these once we store the default values for these
    // fields in the serializer.
    BytecodeArray bytecode_array = BytecodeArray::cast(obj);
    bytecode_array->set_interrupt_budget(
        interpreter::Interpreter::InterruptBudget());
    bytecode_array->set_osr_loop_nesting_level(0);
  }

  // Check alignment.
  DCHECK_EQ(0, Heap::GetFillToAlign(obj->address(),
                                    HeapObject::RequiredAlignment(obj->map())));
  return obj;
}

HeapObject* Deserializer::GetBackReferencedObject(int space) {
  HeapObject* obj;
  switch (space) {
    case LO_SPACE:
      obj = allocator()->GetLargeObject(source_.GetInt());
      break;
    case MAP_SPACE:
      obj = allocator()->GetMap(source_.GetInt());
      break;
    case RO_SPACE: {
      uint32_t chunk_index = source_.GetInt();
      uint32_t chunk_offset = source_.GetInt();
      if (isolate()->heap()->deserialization_complete()) {
        PagedSpace* read_only_space = isolate()->heap()->read_only_space();
        Page* page = read_only_space->first_page();
        for (uint32_t i = 0; i < chunk_index; ++i) {
          page = page->next_page();
        }
        Address address = page->OffsetToAddress(chunk_offset);
        obj = HeapObject::FromAddress(address);
      } else {
        obj = allocator()->GetObject(static_cast<AllocationSpace>(space),
                                     chunk_index, chunk_offset);
      }
      break;
    }
    default: {
      uint32_t chunk_index = source_.GetInt();
      uint32_t chunk_offset = source_.GetInt();
      obj = allocator()->GetObject(static_cast<AllocationSpace>(space),
                                   chunk_index, chunk_offset);
      break;
    }
  }

  if (deserializing_user_code() && obj->IsThinString()) {
    obj = ThinString::cast(obj)->actual();
  }

  hot_objects_.Add(obj);
  DCHECK(!HasWeakHeapObjectTag(obj));
  return obj;
}

// This routine writes the new object into the pointer provided.
// The reason for this strange interface is that otherwise the object is
// written very late, which means the FreeSpace map is not set up by the
// time we need to use it to mark the space at the end of a page free.
void Deserializer::ReadObject(int space_number, UnalignedSlot write_back,
                              HeapObjectReferenceType reference_type) {
  const int size = source_.GetInt() << kObjectAlignmentBits;

  Address address =
      allocator()->Allocate(static_cast<AllocationSpace>(space_number), size);
  HeapObject* obj = HeapObject::FromAddress(address);

  isolate_->heap()->OnAllocationEvent(obj, size);
  UnalignedSlot current(address);
  UnalignedSlot limit(address + size);

  if (ReadData(current, limit, space_number, address)) {
    // Only post process if object content has not been deferred.
    obj = PostProcessNewObject(obj, space_number);
  }

  MaybeObject write_back_obj = reference_type == HeapObjectReferenceType::STRONG
                                   ? HeapObjectReference::Strong(obj)
                                   : HeapObjectReference::Weak(obj);
  UnalignedCopy(write_back, write_back_obj);
#ifdef DEBUG
  if (obj->IsCode()) {
    DCHECK(space_number == CODE_SPACE || space_number == CODE_LO_SPACE);
  } else {
    DCHECK(space_number != CODE_SPACE && space_number != CODE_LO_SPACE);
  }
#endif  // DEBUG
}

static void NoExternalReferencesCallback() {
  // The following check will trigger if a function or object template
  // with references to native functions have been deserialized from
  // snapshot, but no actual external references were provided when the
  // isolate was created.
  CHECK_WITH_MSG(false, "No external references provided via API");
}

bool Deserializer::ReadData(UnalignedSlot current, UnalignedSlot limit,
                            int source_space, Address current_object_address) {
  Isolate* const isolate = isolate_;
  // Write barrier support costs around 1% in startup time.  In fact there
  // are no new space objects in current boot snapshots, so it's not needed,
  // but that may change.
  bool write_barrier_needed =
      (current_object_address != kNullAddress && source_space != NEW_SPACE &&
       source_space != CODE_SPACE);
  while (current < limit) {
    byte data = source_.Get();
    switch (data) {
#define CASE_STATEMENT(where, how, within, space_number) \
  case where + how + within + space_number:              \
    STATIC_ASSERT((where & ~kWhereMask) == 0);           \
    STATIC_ASSERT((how & ~kHowToCodeMask) == 0);         \
    STATIC_ASSERT((within & ~kWhereToPointMask) == 0);   \
    STATIC_ASSERT((space_number & ~kSpaceMask) == 0);

#define CASE_BODY(where, how, within, space_number_if_any)                   \
  current = ReadDataCase<where, how, within, space_number_if_any>(           \
      isolate, current, current_object_address, data, write_barrier_needed); \
  break;

// This generates a case and a body for the new space (which has to do extra
// write barrier handling) and handles the other spaces with fall-through cases
// and one body.
#define ALL_SPACES(where, how, within)           \
  CASE_STATEMENT(where, how, within, NEW_SPACE)  \
  CASE_BODY(where, how, within, NEW_SPACE)       \
  CASE_STATEMENT(where, how, within, OLD_SPACE)  \
  V8_FALLTHROUGH;                                \
  CASE_STATEMENT(where, how, within, CODE_SPACE) \
  V8_FALLTHROUGH;                                \
  CASE_STATEMENT(where, how, within, MAP_SPACE)  \
  V8_FALLTHROUGH;                                \
  CASE_STATEMENT(where, how, within, LO_SPACE)   \
  V8_FALLTHROUGH;                                \
  CASE_STATEMENT(where, how, within, RO_SPACE)   \
  CASE_BODY(where, how, within, kAnyOldSpace)

#define FOUR_CASES(byte_code) \
  case byte_code:             \
  case byte_code + 1:         \
  case byte_code + 2:         \
  case byte_code + 3:

#define SIXTEEN_CASES(byte_code) \
  FOUR_CASES(byte_code)          \
  FOUR_CASES(byte_code + 4)      \
  FOUR_CASES(byte_code + 8)      \
  FOUR_CASES(byte_code + 12)

#define SINGLE_CASE(where, how, within, space) \
  CASE_STATEMENT(where, how, within, space)    \
  CASE_BODY(where, how, within, space)

      // Deserialize a new object and write a pointer to it to the current
      // object.
      ALL_SPACES(kNewObject, kPlain, kStartOfObject)
      // Deserialize a new code object and write a pointer to its first
      // instruction to the current code object.
      ALL_SPACES(kNewObject, kFromCode, kInnerPointer)
      // Find a recently deserialized object using its offset from the current
      // allocation point and write a pointer to it to the current object.
      ALL_SPACES(kBackref, kPlain, kStartOfObject)
      ALL_SPACES(kBackrefWithSkip, kPlain, kStartOfObject)
#if V8_CODE_EMBEDS_OBJECT_POINTER
      // Deserialize a new object from pointer found in code and write
      // a pointer to it to the current object. Required only for MIPS, PPC, ARM
      // or S390 with embedded constant pool, and omitted on the other
      // architectures because it is fully unrolled and would cause bloat.
      ALL_SPACES(kNewObject, kFromCode, kStartOfObject)
      // Find a recently deserialized code object using its offset from the
      // current allocation point and write a pointer to it to the current
      // object. Required only for MIPS, PPC, ARM or S390 with embedded
      // constant pool.
      ALL_SPACES(kBackref, kFromCode, kStartOfObject)
      ALL_SPACES(kBackrefWithSkip, kFromCode, kStartOfObject)
#endif
      // Find a recently deserialized code object using its offset from the
      // current allocation point and write a pointer to its first instruction
      // to the current code object or the instruction pointer in a function
      // object.
      ALL_SPACES(kBackref, kFromCode, kInnerPointer)
      ALL_SPACES(kBackrefWithSkip, kFromCode, kInnerPointer)
      // Find an object in the roots array and write a pointer to it to the
      // current object.
      SINGLE_CASE(kRootArray, kPlain, kStartOfObject, 0)
#if V8_CODE_EMBEDS_OBJECT_POINTER
      // Find an object in the roots array and write a pointer to it to in code.
      SINGLE_CASE(kRootArray, kFromCode, kStartOfObject, 0)
#endif
      // Find an object in the partial snapshots cache and write a pointer to it
      // to the current object.
      SINGLE_CASE(kPartialSnapshotCache, kPlain, kStartOfObject, 0)
      SINGLE_CASE(kPartialSnapshotCache, kFromCode, kStartOfObject, 0)
      SINGLE_CASE(kPartialSnapshotCache, kFromCode, kInnerPointer, 0)
      // Find an object in the partial snapshots cache and write a pointer to it
      // to the current object.
      SINGLE_CASE(kReadOnlyObjectCache, kPlain, kStartOfObject, 0)
      SINGLE_CASE(kReadOnlyObjectCache, kFromCode, kStartOfObject, 0)
      SINGLE_CASE(kReadOnlyObjectCache, kFromCode, kInnerPointer, 0)
      // Find an object in the attached references and write a pointer to it to
      // the current object.
      SINGLE_CASE(kAttachedReference, kPlain, kStartOfObject, 0)
      SINGLE_CASE(kAttachedReference, kFromCode, kStartOfObject, 0)
      SINGLE_CASE(kAttachedReference, kFromCode, kInnerPointer, 0)

#undef CASE_STATEMENT
#undef CASE_BODY
#undef ALL_SPACES

      case kSkip: {
        int size = source_.GetInt();
        current.Advance(size);
        break;
      }

      // Find an external reference and write a pointer to it to the current
      // object.
      case kExternalReference + kPlain + kStartOfObject:
        current =
            ReadExternalReferenceCase(kPlain, current, current_object_address);
        break;
      // Find an external reference and write a pointer to it in the current
      // code object.
      case kExternalReference + kFromCode + kStartOfObject:
        current = ReadExternalReferenceCase(kFromCode, current,
                                            current_object_address);
        break;

      case kInternalReferenceEncoded:
      case kInternalReference: {
        // Internal reference address is not encoded via skip, but by offset
        // from code entry.
        int pc_offset = source_.GetInt();
        int target_offset = source_.GetInt();
        Code code = Code::cast(HeapObject::FromAddress(current_object_address));
        DCHECK(0 <= pc_offset && pc_offset <= code->raw_instruction_size());
        DCHECK(0 <= target_offset &&
               target_offset <= code->raw_instruction_size());
        Address pc = code->entry() + pc_offset;
        Address target = code->entry() + target_offset;
        Assembler::deserialization_set_target_internal_reference_at(
            pc, target,
            data == kInternalReference ? RelocInfo::INTERNAL_REFERENCE
                                       : RelocInfo::INTERNAL_REFERENCE_ENCODED);
        break;
      }

      case kOffHeapTarget: {
        DCHECK(FLAG_embedded_builtins);
        int skip = source_.GetInt();
        int builtin_index = source_.GetInt();
        DCHECK(Builtins::IsBuiltinId(builtin_index));

        current.Advance(skip);

        CHECK_NOT_NULL(isolate->embedded_blob());
        EmbeddedData d = EmbeddedData::FromBlob();
        Address address = d.InstructionStartOfBuiltin(builtin_index);
        CHECK_NE(kNullAddress, address);

        if (RelocInfo::OffHeapTargetIsCodedSpecially()) {
          Address location_of_branch_data = current.address();
          int skip = Assembler::deserialization_special_target_size(
              location_of_branch_data);
          Assembler::deserialization_set_special_target_at(
              location_of_branch_data,
              Code::cast(HeapObject::FromAddress(current_object_address)),
              address);
          current.Advance(skip);
        } else {
          UnalignedCopy(current, address);
          current.Advance();
        }
        break;
      }

      case kNop:
        break;

      case kNextChunk: {
        int space = source_.Get();
        allocator()->MoveToNextChunk(static_cast<AllocationSpace>(space));
        break;
      }

      case kDeferred: {
        // Deferred can only occur right after the heap object header.
        DCHECK_EQ(current.address(), current_object_address + kPointerSize);
        HeapObject* obj = HeapObject::FromAddress(current_object_address);
        // If the deferred object is a map, its instance type may be used
        // during deserialization. Initialize it with a temporary value.
        if (obj->IsMap()) Map::cast(obj)->set_instance_type(FILLER_TYPE);
        current = limit;
        return false;
      }

      case kSynchronize:
        // If we get here then that indicates that you have a mismatch between
        // the number of GC roots when serializing and deserializing.
        UNREACHABLE();

      // Deserialize raw data of variable length.
      case kVariableRawData: {
        int size_in_bytes = source_.GetInt();
        byte* raw_data_out = reinterpret_cast<byte*>(current.address());
        source_.CopyRaw(raw_data_out, size_in_bytes);
        current.Advance(size_in_bytes);
        break;
      }

      // Deserialize raw code directly into the body of the code object.
      // Do not move current.
      case kVariableRawCode: {
        int size_in_bytes = source_.GetInt();
        source_.CopyRaw(
            reinterpret_cast<byte*>(current_object_address + Code::kDataStart),
            size_in_bytes);
        break;
      }

      case kVariableRepeat: {
        int repeats = source_.GetInt();
        MaybeObject object = current.ReadPrevious();
        DCHECK(!Heap::InNewSpace(object));
        for (int i = 0; i < repeats; i++) {
          UnalignedCopy(current, object);
          current.Advance();
        }
        break;
      }

      case kOffHeapBackingStore: {
        int byte_length = source_.GetInt();
        byte* backing_store = static_cast<byte*>(
            isolate->array_buffer_allocator()->AllocateUninitialized(
                byte_length));
        CHECK_NOT_NULL(backing_store);
        source_.CopyRaw(backing_store, byte_length);
        off_heap_backing_stores_.push_back(backing_store);
        break;
      }

      case kApiReference: {
        int skip = source_.GetInt();
        current.Advance(skip);
        uint32_t reference_id = static_cast<uint32_t>(source_.GetInt());
        Address address;
        if (isolate->api_external_references()) {
          DCHECK_WITH_MSG(
              reference_id < num_api_references_,
              "too few external references provided through the API");
          address = static_cast<Address>(
              isolate->api_external_references()[reference_id]);
        } else {
          address = reinterpret_cast<Address>(NoExternalReferencesCallback);
        }
        UnalignedCopy(current, address);
        current.Advance();
        break;
      }

      case kClearedWeakReference:
        UnalignedCopy(current, HeapObjectReference::ClearedValue(isolate_));
        current.Advance();
        break;

      case kWeakPrefix:
        DCHECK(!allocator()->next_reference_is_weak());
        allocator()->set_next_reference_is_weak(true);
        break;

      case kAlignmentPrefix:
      case kAlignmentPrefix + 1:
      case kAlignmentPrefix + 2: {
        int alignment = data - (SerializerDeserializer::kAlignmentPrefix - 1);
        allocator()->SetAlignment(static_cast<AllocationAlignment>(alignment));
        break;
      }

      // First kNumberOfRootArrayConstants roots are guaranteed to be in
      // the old space.
      STATIC_ASSERT(
          static_cast<int>(RootIndex::kFirstImmortalImmovableRoot) == 0);
      STATIC_ASSERT(kNumberOfRootArrayConstants <=
                    static_cast<int>(RootIndex::kLastImmortalImmovableRoot));
      STATIC_ASSERT(kNumberOfRootArrayConstants == 32);
      SIXTEEN_CASES(kRootArrayConstantsWithSkip)
      SIXTEEN_CASES(kRootArrayConstantsWithSkip + 16) {
        int skip = source_.GetInt();
        current.Advance(skip);
        V8_FALLTHROUGH;
      }

      SIXTEEN_CASES(kRootArrayConstants)
      SIXTEEN_CASES(kRootArrayConstants + 16) {
        int id = data & kRootArrayConstantsMask;
        RootIndex root_index = static_cast<RootIndex>(id);
        MaybeObject object = MaybeObject::FromObject(isolate->root(root_index));
        DCHECK(!Heap::InNewSpace(object));
        UnalignedCopy(current, object);
        current.Advance();
        break;
      }

      STATIC_ASSERT(kNumberOfHotObjects == 8);
      FOUR_CASES(kHotObjectWithSkip)
      FOUR_CASES(kHotObjectWithSkip + 4) {
        int skip = source_.GetInt();
        current.Advance(skip);
        V8_FALLTHROUGH;
      }

      FOUR_CASES(kHotObject)
      FOUR_CASES(kHotObject + 4) {
        int index = data & kHotObjectMask;
        Object* hot_object = hot_objects_.Get(index);
        MaybeObject hot_maybe_object = MaybeObject::FromObject(hot_object);
        if (allocator()->GetAndClearNextReferenceIsWeak()) {
          hot_maybe_object = MaybeObject::MakeWeak(hot_maybe_object);
        }

        UnalignedCopy(current, hot_maybe_object);
        if (write_barrier_needed && Heap::InNewSpace(hot_object)) {
          GenerationalBarrier(HeapObject::FromAddress(current_object_address),
                              current.Slot(), hot_maybe_object);
        }
        current.Advance();
        break;
      }

      // Deserialize raw data of fixed length from 1 to 32 words.
      STATIC_ASSERT(kNumberOfFixedRawData == 32);
      SIXTEEN_CASES(kFixedRawData)
      SIXTEEN_CASES(kFixedRawData + 16) {
        byte* raw_data_out = reinterpret_cast<byte*>(current.address());
        int size_in_bytes = (data - kFixedRawDataStart) << kPointerSizeLog2;
        source_.CopyRaw(raw_data_out, size_in_bytes);
        current.Advance(size_in_bytes);
        break;
      }

      STATIC_ASSERT(kNumberOfFixedRepeat == 16);
      SIXTEEN_CASES(kFixedRepeat) {
        int repeats = data - kFixedRepeatStart;
        MaybeObject object = current.ReadPrevious();
        DCHECK(!Heap::InNewSpace(object));
        for (int i = 0; i < repeats; i++) {
          UnalignedCopy(current, object);
          current.Advance();
        }
        break;
      }

#ifdef DEBUG
#define UNUSED_CASE(byte_code) \
  case byte_code:              \
    UNREACHABLE();
      UNUSED_SERIALIZER_BYTE_CODES(UNUSED_CASE)
#endif
#undef UNUSED_CASE

#undef SIXTEEN_CASES
#undef FOUR_CASES
#undef SINGLE_CASE
    }
  }
  CHECK_EQ(limit, current);
  return true;
}

UnalignedSlot Deserializer::ReadExternalReferenceCase(
    HowToCode how, UnalignedSlot current, Address current_object_address) {
  int skip = source_.GetInt();
  current.Advance(skip);
  uint32_t reference_id = static_cast<uint32_t>(source_.GetInt());
  Address address = external_reference_table_->address(reference_id);

  if (how == kFromCode) {
    Address location_of_branch_data = current.address();
    int skip =
        Assembler::deserialization_special_target_size(location_of_branch_data);
    Assembler::deserialization_set_special_target_at(
        location_of_branch_data,
        Code::cast(HeapObject::FromAddress(current_object_address)), address);
    current.Advance(skip);
  } else {
    UnalignedCopy(current, address);
    current.Advance();
  }
  return current;
}

template <int where, int how, int within, int space_number_if_any>
UnalignedSlot Deserializer::ReadDataCase(Isolate* isolate,
                                         UnalignedSlot current,
                                         Address current_object_address,
                                         byte data, bool write_barrier_needed) {
  bool emit_write_barrier = false;
  bool current_was_incremented = false;
  int space_number = space_number_if_any == kAnyOldSpace ? (data & kSpaceMask)
                                                         : space_number_if_any;
  HeapObjectReferenceType reference_type = HeapObjectReferenceType::STRONG;
  if (where == kNewObject && how == kPlain && within == kStartOfObject) {
    if (allocator()->GetAndClearNextReferenceIsWeak()) {
      reference_type = HeapObjectReferenceType::WEAK;
    }
    ReadObject(space_number, current, reference_type);
    emit_write_barrier = (space_number == NEW_SPACE);
  } else {
    Object* new_object = nullptr; /* May not be a real Object pointer. */
    if (where == kNewObject) {
      ReadObject(space_number, UnalignedSlot(&new_object),
                 HeapObjectReferenceType::STRONG);
    } else if (where == kBackref) {
      emit_write_barrier = (space_number == NEW_SPACE);
      new_object = GetBackReferencedObject(data & kSpaceMask);
    } else if (where == kBackrefWithSkip) {
      int skip = source_.GetInt();
      current.Advance(skip);
      emit_write_barrier = (space_number == NEW_SPACE);
      new_object = GetBackReferencedObject(data & kSpaceMask);
    } else if (where == kRootArray) {
      int id = source_.GetInt();
      RootIndex root_index = static_cast<RootIndex>(id);
      new_object = isolate->root(root_index);
      emit_write_barrier = Heap::InNewSpace(new_object);
      hot_objects_.Add(HeapObject::cast(new_object));
    } else if (where == kReadOnlyObjectCache) {
      int cache_index = source_.GetInt();
      new_object = isolate->read_only_object_cache()->at(cache_index);
      DCHECK(!Heap::InNewSpace(new_object));
      emit_write_barrier = false;
    } else if (where == kPartialSnapshotCache) {
      int cache_index = source_.GetInt();
      new_object = isolate->partial_snapshot_cache()->at(cache_index);
      emit_write_barrier = Heap::InNewSpace(new_object);
    } else {
      DCHECK_EQ(where, kAttachedReference);
      int index = source_.GetInt();
      new_object = *attached_objects_[index];
      emit_write_barrier = Heap::InNewSpace(new_object);
    }
    if (within == kInnerPointer) {
      DCHECK_EQ(how, kFromCode);
      if (new_object->IsCode()) {
        new_object = reinterpret_cast<Object*>(
            Code::cast(new_object)->raw_instruction_start());
      } else {
        Cell cell = Cell::cast(new_object);
        new_object = reinterpret_cast<Object*>(cell->ValueAddress());
      }
    }
    if (how == kFromCode) {
      DCHECK(!allocator()->next_reference_is_weak());
      Address location_of_branch_data = current.address();
      int skip = Assembler::deserialization_special_target_size(
          location_of_branch_data);
      Assembler::deserialization_set_special_target_at(
          location_of_branch_data,
          Code::cast(HeapObject::FromAddress(current_object_address)),
          reinterpret_cast<Address>(new_object));
      current.Advance(skip);
      current_was_incremented = true;
    } else {
      MaybeObject new_maybe_object = MaybeObject::FromObject(new_object);
      if (allocator()->GetAndClearNextReferenceIsWeak()) {
        new_maybe_object = MaybeObject::MakeWeak(new_maybe_object);
      }
      UnalignedCopy(current, new_maybe_object);
    }
  }
  if (emit_write_barrier && write_barrier_needed) {
    HeapObject* object = HeapObject::FromAddress(current_object_address);
    SLOW_DCHECK(isolate->heap()->Contains(object));
    GenerationalBarrier(object, current.Slot(), current.Read());
  }
  if (!current_was_incremented) {
    current.Advance();
  }

  return current;
}

}  // namespace internal
}  // namespace v8
