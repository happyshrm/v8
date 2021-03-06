// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/profiler/heap-snapshot-generator.h"

#include <utility>

#include "src/api.h"
#include "src/code-stubs.h"
#include "src/conversions.h"
#include "src/debug/debug.h"
#include "src/global-handles.h"
#include "src/layout-descriptor.h"
#include "src/objects-body-descriptors.h"
#include "src/objects-inl.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/js-collection-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/js-regexp-inl.h"
#include "src/objects/literal-objects-inl.h"
#include "src/profiler/allocation-tracker.h"
#include "src/profiler/heap-profiler.h"
#include "src/profiler/heap-snapshot-generator-inl.h"
#include "src/prototype.h"
#include "src/transitions.h"
#include "src/visitors.h"

namespace v8 {
namespace internal {


HeapGraphEdge::HeapGraphEdge(Type type, const char* name, int from, int to)
    : bit_field_(TypeField::encode(type) | FromIndexField::encode(from)),
      to_index_(to),
      name_(name) {
  DCHECK(type == kContextVariable
      || type == kProperty
      || type == kInternal
      || type == kShortcut
      || type == kWeak);
}


HeapGraphEdge::HeapGraphEdge(Type type, int index, int from, int to)
    : bit_field_(TypeField::encode(type) | FromIndexField::encode(from)),
      to_index_(to),
      index_(index) {
  DCHECK(type == kElement || type == kHidden);
}


void HeapGraphEdge::ReplaceToIndexWithEntry(HeapSnapshot* snapshot) {
  to_entry_ = &snapshot->entries()[to_index_];
}


const int HeapEntry::kNoEntry = -1;

HeapEntry::HeapEntry(HeapSnapshot* snapshot,
                     Type type,
                     const char* name,
                     SnapshotObjectId id,
                     size_t self_size,
                     unsigned trace_node_id)
    : type_(type),
      children_count_(0),
      children_index_(-1),
      self_size_(self_size),
      snapshot_(snapshot),
      name_(name),
      id_(id),
      trace_node_id_(trace_node_id) { }


void HeapEntry::SetNamedReference(HeapGraphEdge::Type type,
                                  const char* name,
                                  HeapEntry* entry) {
  HeapGraphEdge edge(type, name, this->index(), entry->index());
  snapshot_->edges().push_back(edge);
  ++children_count_;
}


void HeapEntry::SetIndexedReference(HeapGraphEdge::Type type,
                                    int index,
                                    HeapEntry* entry) {
  HeapGraphEdge edge(type, index, this->index(), entry->index());
  snapshot_->edges().push_back(edge);
  ++children_count_;
}


void HeapEntry::Print(
    const char* prefix, const char* edge_name, int max_depth, int indent) {
  STATIC_ASSERT(sizeof(unsigned) == sizeof(id()));
  base::OS::Print("%6" PRIuS " @%6u %*c %s%s: ", self_size(), id(), indent, ' ',
                  prefix, edge_name);
  if (type() != kString) {
    base::OS::Print("%s %.40s\n", TypeAsString(), name_);
  } else {
    base::OS::Print("\"");
    const char* c = name_;
    while (*c && (c - name_) <= 40) {
      if (*c != '\n')
        base::OS::Print("%c", *c);
      else
        base::OS::Print("\\n");
      ++c;
    }
    base::OS::Print("\"\n");
  }
  if (--max_depth == 0) return;
  for (auto i = children_begin(); i != children_end(); ++i) {
    HeapGraphEdge& edge = **i;
    const char* edge_prefix = "";
    EmbeddedVector<char, 64> index;
    const char* edge_name = index.start();
    switch (edge.type()) {
      case HeapGraphEdge::kContextVariable:
        edge_prefix = "#";
        edge_name = edge.name();
        break;
      case HeapGraphEdge::kElement:
        SNPrintF(index, "%d", edge.index());
        break;
      case HeapGraphEdge::kInternal:
        edge_prefix = "$";
        edge_name = edge.name();
        break;
      case HeapGraphEdge::kProperty:
        edge_name = edge.name();
        break;
      case HeapGraphEdge::kHidden:
        edge_prefix = "$";
        SNPrintF(index, "%d", edge.index());
        break;
      case HeapGraphEdge::kShortcut:
        edge_prefix = "^";
        edge_name = edge.name();
        break;
      case HeapGraphEdge::kWeak:
        edge_prefix = "w";
        edge_name = edge.name();
        break;
      default:
        SNPrintF(index, "!!! unknown edge type: %d ", edge.type());
    }
    edge.to()->Print(edge_prefix, edge_name, max_depth, indent + 2);
  }
}


const char* HeapEntry::TypeAsString() {
  switch (type()) {
    case kHidden: return "/hidden/";
    case kObject: return "/object/";
    case kClosure: return "/closure/";
    case kString: return "/string/";
    case kCode: return "/code/";
    case kArray: return "/array/";
    case kRegExp: return "/regexp/";
    case kHeapNumber: return "/number/";
    case kNative: return "/native/";
    case kSynthetic: return "/synthetic/";
    case kConsString: return "/concatenated string/";
    case kSlicedString: return "/sliced string/";
    case kSymbol: return "/symbol/";
    case kBigInt:
      return "/bigint/";
    default: return "???";
  }
}


HeapSnapshot::HeapSnapshot(HeapProfiler* profiler)
    : profiler_(profiler),
      root_index_(HeapEntry::kNoEntry),
      gc_roots_index_(HeapEntry::kNoEntry),
      max_snapshot_js_object_id_(0) {
  // It is very important to keep objects that form a heap snapshot
  // as small as possible. Check assumptions about data structure sizes.
  STATIC_ASSERT(((kPointerSize == 4) && (sizeof(HeapGraphEdge) == 12)) ||
                ((kPointerSize == 8) && (sizeof(HeapGraphEdge) == 24)));
  STATIC_ASSERT(((kPointerSize == 4) && (sizeof(HeapEntry) == 28)) ||
                ((kPointerSize == 8) && (sizeof(HeapEntry) == 40)));
  for (int i = 0; i < static_cast<int>(Root::kNumberOfRoots); ++i) {
    gc_subroot_indexes_[i] = HeapEntry::kNoEntry;
  }
}


void HeapSnapshot::Delete() {
  profiler_->RemoveSnapshot(this);
}


void HeapSnapshot::RememberLastJSObjectId() {
  max_snapshot_js_object_id_ = profiler_->heap_object_map()->last_assigned_id();
}


void HeapSnapshot::AddSyntheticRootEntries() {
  AddRootEntry();
  AddGcRootsEntry();
  SnapshotObjectId id = HeapObjectsMap::kGcRootsFirstSubrootId;
  for (int root = 0; root < static_cast<int>(Root::kNumberOfRoots); root++) {
    AddGcSubrootEntry(static_cast<Root>(root), id);
    id += HeapObjectsMap::kObjectIdStep;
  }
  DCHECK_EQ(HeapObjectsMap::kFirstAvailableObjectId, id);
}


HeapEntry* HeapSnapshot::AddRootEntry() {
  DCHECK_EQ(root_index_, HeapEntry::kNoEntry);
  DCHECK(entries_.empty());  // Root entry must be the first one.
  HeapEntry* entry = AddEntry(HeapEntry::kSynthetic,
                              "",
                              HeapObjectsMap::kInternalRootObjectId,
                              0,
                              0);
  root_index_ = entry->index();
  DCHECK_EQ(root_index_, 0);
  return entry;
}


HeapEntry* HeapSnapshot::AddGcRootsEntry() {
  DCHECK_EQ(gc_roots_index_, HeapEntry::kNoEntry);
  HeapEntry* entry = AddEntry(HeapEntry::kSynthetic,
                              "(GC roots)",
                              HeapObjectsMap::kGcRootsObjectId,
                              0,
                              0);
  gc_roots_index_ = entry->index();
  return entry;
}

HeapEntry* HeapSnapshot::AddGcSubrootEntry(Root root, SnapshotObjectId id) {
  DCHECK_EQ(gc_subroot_indexes_[static_cast<int>(root)], HeapEntry::kNoEntry);
  HeapEntry* entry =
      AddEntry(HeapEntry::kSynthetic, RootVisitor::RootName(root), id, 0, 0);
  gc_subroot_indexes_[static_cast<int>(root)] = entry->index();
  return entry;
}


HeapEntry* HeapSnapshot::AddEntry(HeapEntry::Type type,
                                  const char* name,
                                  SnapshotObjectId id,
                                  size_t size,
                                  unsigned trace_node_id) {
  DCHECK(sorted_entries_.empty());
  entries_.emplace_back(this, type, name, id, size, trace_node_id);
  return &entries_.back();
}


void HeapSnapshot::FillChildren() {
  DCHECK(children().empty());
  children().resize(edges().size());
  int children_index = 0;
  for (HeapEntry& entry : entries()) {
    children_index = entry.set_children_index(children_index);
  }
  DCHECK_EQ(edges().size(), static_cast<size_t>(children_index));
  for (HeapGraphEdge& edge : edges()) {
    edge.ReplaceToIndexWithEntry(this);
    edge.from()->add_child(&edge);
  }
}

HeapEntry* HeapSnapshot::GetEntryById(SnapshotObjectId id) {
  std::vector<HeapEntry*>* entries_by_id = GetSortedEntriesList();

  auto it = std::lower_bound(
      entries_by_id->begin(), entries_by_id->end(), id,
      [](HeapEntry* first, SnapshotObjectId val) { return first->id() < val; });

  if (it == entries_by_id->end() || (*it)->id() != id) return nullptr;
  return *it;
}

struct SortByIds {
  bool operator()(const HeapEntry* entry1_ptr, const HeapEntry* entry2_ptr) {
    return entry1_ptr->id() < entry2_ptr->id();
  }
};

std::vector<HeapEntry*>* HeapSnapshot::GetSortedEntriesList() {
  if (sorted_entries_.empty()) {
    sorted_entries_.reserve(entries_.size());
    for (HeapEntry& entry : entries_) {
      sorted_entries_.push_back(&entry);
    }
    std::sort(sorted_entries_.begin(), sorted_entries_.end(), SortByIds());
  }
  return &sorted_entries_;
}

void HeapSnapshot::Print(int max_depth) {
  root()->Print("", "", max_depth, 0);
}

// We split IDs on evens for embedder objects (see
// HeapObjectsMap::GenerateId) and odds for native objects.
const SnapshotObjectId HeapObjectsMap::kInternalRootObjectId = 1;
const SnapshotObjectId HeapObjectsMap::kGcRootsObjectId =
    HeapObjectsMap::kInternalRootObjectId + HeapObjectsMap::kObjectIdStep;
const SnapshotObjectId HeapObjectsMap::kGcRootsFirstSubrootId =
    HeapObjectsMap::kGcRootsObjectId + HeapObjectsMap::kObjectIdStep;
const SnapshotObjectId HeapObjectsMap::kFirstAvailableObjectId =
    HeapObjectsMap::kGcRootsFirstSubrootId +
    static_cast<int>(Root::kNumberOfRoots) * HeapObjectsMap::kObjectIdStep;

HeapObjectsMap::HeapObjectsMap(Heap* heap)
    : next_id_(kFirstAvailableObjectId), heap_(heap) {
  // The dummy element at zero index is needed as entries_map_ cannot hold
  // an entry with zero value. Otherwise it's impossible to tell if
  // LookupOrInsert has added a new item or just returning exisiting one
  // having the value of zero.
  entries_.emplace_back(0, kNullAddress, 0, true);
}

bool HeapObjectsMap::MoveObject(Address from, Address to, int object_size) {
  DCHECK_NE(kNullAddress, to);
  DCHECK_NE(kNullAddress, from);
  if (from == to) return false;
  void* from_value = entries_map_.Remove(reinterpret_cast<void*>(from),
                                         ComputeAddressHash(from));
  if (from_value == nullptr) {
    // It may occur that some untracked object moves to an address X and there
    // is a tracked object at that address. In this case we should remove the
    // entry as we know that the object has died.
    void* to_value = entries_map_.Remove(reinterpret_cast<void*>(to),
                                         ComputeAddressHash(to));
    if (to_value != nullptr) {
      int to_entry_info_index =
          static_cast<int>(reinterpret_cast<intptr_t>(to_value));
      entries_.at(to_entry_info_index).addr = kNullAddress;
    }
  } else {
    base::HashMap::Entry* to_entry = entries_map_.LookupOrInsert(
        reinterpret_cast<void*>(to), ComputeAddressHash(to));
    if (to_entry->value != nullptr) {
      // We found the existing entry with to address for an old object.
      // Without this operation we will have two EntryInfo's with the same
      // value in addr field. It is bad because later at RemoveDeadEntries
      // one of this entry will be removed with the corresponding entries_map_
      // entry.
      int to_entry_info_index =
          static_cast<int>(reinterpret_cast<intptr_t>(to_entry->value));
      entries_.at(to_entry_info_index).addr = kNullAddress;
    }
    int from_entry_info_index =
        static_cast<int>(reinterpret_cast<intptr_t>(from_value));
    entries_.at(from_entry_info_index).addr = to;
    // Size of an object can change during its life, so to keep information
    // about the object in entries_ consistent, we have to adjust size when the
    // object is migrated.
    if (FLAG_heap_profiler_trace_objects) {
      PrintF("Move object from %p to %p old size %6d new size %6d\n",
             reinterpret_cast<void*>(from), reinterpret_cast<void*>(to),
             entries_.at(from_entry_info_index).size, object_size);
    }
    entries_.at(from_entry_info_index).size = object_size;
    to_entry->value = from_value;
  }
  return from_value != nullptr;
}


void HeapObjectsMap::UpdateObjectSize(Address addr, int size) {
  FindOrAddEntry(addr, size, false);
}


SnapshotObjectId HeapObjectsMap::FindEntry(Address addr) {
  base::HashMap::Entry* entry = entries_map_.Lookup(
      reinterpret_cast<void*>(addr), ComputeAddressHash(addr));
  if (entry == nullptr) return 0;
  int entry_index = static_cast<int>(reinterpret_cast<intptr_t>(entry->value));
  EntryInfo& entry_info = entries_.at(entry_index);
  DCHECK(static_cast<uint32_t>(entries_.size()) > entries_map_.occupancy());
  return entry_info.id;
}


SnapshotObjectId HeapObjectsMap::FindOrAddEntry(Address addr,
                                                unsigned int size,
                                                bool accessed) {
  DCHECK(static_cast<uint32_t>(entries_.size()) > entries_map_.occupancy());
  base::HashMap::Entry* entry = entries_map_.LookupOrInsert(
      reinterpret_cast<void*>(addr), ComputeAddressHash(addr));
  if (entry->value != nullptr) {
    int entry_index =
        static_cast<int>(reinterpret_cast<intptr_t>(entry->value));
    EntryInfo& entry_info = entries_.at(entry_index);
    entry_info.accessed = accessed;
    if (FLAG_heap_profiler_trace_objects) {
      PrintF("Update object size : %p with old size %d and new size %d\n",
             reinterpret_cast<void*>(addr), entry_info.size, size);
    }
    entry_info.size = size;
    return entry_info.id;
  }
  entry->value = reinterpret_cast<void*>(entries_.size());
  SnapshotObjectId id = next_id_;
  next_id_ += kObjectIdStep;
  entries_.push_back(EntryInfo(id, addr, size, accessed));
  DCHECK(static_cast<uint32_t>(entries_.size()) > entries_map_.occupancy());
  return id;
}

void HeapObjectsMap::StopHeapObjectsTracking() { time_intervals_.clear(); }

void HeapObjectsMap::UpdateHeapObjectsMap() {
  if (FLAG_heap_profiler_trace_objects) {
    PrintF("Begin HeapObjectsMap::UpdateHeapObjectsMap. map has %d entries.\n",
           entries_map_.occupancy());
  }
  heap_->CollectAllGarbage(Heap::kMakeHeapIterableMask,
                           GarbageCollectionReason::kHeapProfiler);
  HeapIterator iterator(heap_);
  for (HeapObject* obj = iterator.next(); obj != nullptr;
       obj = iterator.next()) {
    FindOrAddEntry(obj->address(), obj->Size());
    if (FLAG_heap_profiler_trace_objects) {
      PrintF("Update object      : %p %6d. Next address is %p\n",
             reinterpret_cast<void*>(obj->address()), obj->Size(),
             reinterpret_cast<void*>(obj->address() + obj->Size()));
    }
  }
  RemoveDeadEntries();
  if (FLAG_heap_profiler_trace_objects) {
    PrintF("End HeapObjectsMap::UpdateHeapObjectsMap. map has %d entries.\n",
           entries_map_.occupancy());
  }
}

SnapshotObjectId HeapObjectsMap::PushHeapObjectsStats(OutputStream* stream,
                                                      int64_t* timestamp_us) {
  UpdateHeapObjectsMap();
  time_intervals_.emplace_back(next_id_);
  int prefered_chunk_size = stream->GetChunkSize();
  std::vector<v8::HeapStatsUpdate> stats_buffer;
  DCHECK(!entries_.empty());
  EntryInfo* entry_info = &entries_.front();
  EntryInfo* end_entry_info = &entries_.back() + 1;
  for (size_t time_interval_index = 0;
       time_interval_index < time_intervals_.size(); ++time_interval_index) {
    TimeInterval& time_interval = time_intervals_[time_interval_index];
    SnapshotObjectId time_interval_id = time_interval.id;
    uint32_t entries_size = 0;
    EntryInfo* start_entry_info = entry_info;
    while (entry_info < end_entry_info && entry_info->id < time_interval_id) {
      entries_size += entry_info->size;
      ++entry_info;
    }
    uint32_t entries_count =
        static_cast<uint32_t>(entry_info - start_entry_info);
    if (time_interval.count != entries_count ||
        time_interval.size != entries_size) {
      stats_buffer.emplace_back(static_cast<uint32_t>(time_interval_index),
                                time_interval.count = entries_count,
                                time_interval.size = entries_size);
      if (static_cast<int>(stats_buffer.size()) >= prefered_chunk_size) {
        OutputStream::WriteResult result = stream->WriteHeapStatsChunk(
            &stats_buffer.front(), static_cast<int>(stats_buffer.size()));
        if (result == OutputStream::kAbort) return last_assigned_id();
        stats_buffer.clear();
      }
    }
  }
  DCHECK(entry_info == end_entry_info);
  if (!stats_buffer.empty()) {
    OutputStream::WriteResult result = stream->WriteHeapStatsChunk(
        &stats_buffer.front(), static_cast<int>(stats_buffer.size()));
    if (result == OutputStream::kAbort) return last_assigned_id();
  }
  stream->EndOfStream();
  if (timestamp_us) {
    *timestamp_us =
        (time_intervals_.back().timestamp - time_intervals_.front().timestamp)
            .InMicroseconds();
  }
  return last_assigned_id();
}


void HeapObjectsMap::RemoveDeadEntries() {
  DCHECK(entries_.size() > 0 && entries_.at(0).id == 0 &&
         entries_.at(0).addr == kNullAddress);
  size_t first_free_entry = 1;
  for (size_t i = 1; i < entries_.size(); ++i) {
    EntryInfo& entry_info = entries_.at(i);
    if (entry_info.accessed) {
      if (first_free_entry != i) {
        entries_.at(first_free_entry) = entry_info;
      }
      entries_.at(first_free_entry).accessed = false;
      base::HashMap::Entry* entry =
          entries_map_.Lookup(reinterpret_cast<void*>(entry_info.addr),
                              ComputeAddressHash(entry_info.addr));
      DCHECK(entry);
      entry->value = reinterpret_cast<void*>(first_free_entry);
      ++first_free_entry;
    } else {
      if (entry_info.addr) {
        entries_map_.Remove(reinterpret_cast<void*>(entry_info.addr),
                            ComputeAddressHash(entry_info.addr));
      }
    }
  }
  entries_.erase(entries_.begin() + first_free_entry, entries_.end());

  DCHECK(static_cast<uint32_t>(entries_.size()) - 1 ==
         entries_map_.occupancy());
}


SnapshotObjectId HeapObjectsMap::GenerateId(v8::RetainedObjectInfo* info) {
  SnapshotObjectId id = static_cast<SnapshotObjectId>(info->GetHash());
  const char* label = info->GetLabel();
  id ^= StringHasher::HashSequentialString(label,
                                           static_cast<int>(strlen(label)),
                                           heap_->HashSeed());
  intptr_t element_count = info->GetElementCount();
  if (element_count != -1) {
    id ^= ComputeIntegerHash(static_cast<uint32_t>(element_count));
  }
  return id << 1;
}

HeapEntriesMap::HeapEntriesMap() : entries_() {}

int HeapEntriesMap::Map(HeapThing thing) {
  base::HashMap::Entry* cache_entry = entries_.Lookup(thing, Hash(thing));
  if (cache_entry == nullptr) return HeapEntry::kNoEntry;
  return static_cast<int>(reinterpret_cast<intptr_t>(cache_entry->value));
}


void HeapEntriesMap::Pair(HeapThing thing, int entry) {
  base::HashMap::Entry* cache_entry =
      entries_.LookupOrInsert(thing, Hash(thing));
  DCHECK_NULL(cache_entry->value);
  cache_entry->value = reinterpret_cast<void*>(static_cast<intptr_t>(entry));
}

HeapObjectsSet::HeapObjectsSet() : entries_() {}

void HeapObjectsSet::Clear() {
  entries_.Clear();
}


bool HeapObjectsSet::Contains(Object* obj) {
  if (!obj->IsHeapObject()) return false;
  HeapObject* object = HeapObject::cast(obj);
  return entries_.Lookup(object, HeapEntriesMap::Hash(object)) != nullptr;
}


void HeapObjectsSet::Insert(Object* obj) {
  if (!obj->IsHeapObject()) return;
  HeapObject* object = HeapObject::cast(obj);
  entries_.LookupOrInsert(object, HeapEntriesMap::Hash(object));
}


const char* HeapObjectsSet::GetTag(Object* obj) {
  HeapObject* object = HeapObject::cast(obj);
  base::HashMap::Entry* cache_entry =
      entries_.Lookup(object, HeapEntriesMap::Hash(object));
  return cache_entry != nullptr
             ? reinterpret_cast<const char*>(cache_entry->value)
             : nullptr;
}


V8_NOINLINE void HeapObjectsSet::SetTag(Object* obj, const char* tag) {
  if (!obj->IsHeapObject()) return;
  HeapObject* object = HeapObject::cast(obj);
  base::HashMap::Entry* cache_entry =
      entries_.LookupOrInsert(object, HeapEntriesMap::Hash(object));
  cache_entry->value = const_cast<char*>(tag);
}

V8HeapExplorer::V8HeapExplorer(HeapSnapshot* snapshot,
                               SnapshottingProgressReportingInterface* progress,
                               v8::HeapProfiler::ObjectNameResolver* resolver)
    : heap_(snapshot->profiler()->heap_object_map()->heap()),
      snapshot_(snapshot),
      names_(snapshot_->profiler()->names()),
      heap_object_map_(snapshot_->profiler()->heap_object_map()),
      progress_(progress),
      filler_(nullptr),
      global_object_name_resolver_(resolver) {}

V8HeapExplorer::~V8HeapExplorer() {
}


HeapEntry* V8HeapExplorer::AllocateEntry(HeapThing ptr) {
  return AddEntry(reinterpret_cast<HeapObject*>(ptr));
}


HeapEntry* V8HeapExplorer::AddEntry(HeapObject* object) {
  if (object->IsJSFunction()) {
    JSFunction* func = JSFunction::cast(object);
    SharedFunctionInfo* shared = func->shared();
    const char* name = names_->GetName(shared->Name());
    return AddEntry(object, HeapEntry::kClosure, name);
  } else if (object->IsJSBoundFunction()) {
    return AddEntry(object, HeapEntry::kClosure, "native_bind");
  } else if (object->IsJSRegExp()) {
    JSRegExp* re = JSRegExp::cast(object);
    return AddEntry(object,
                    HeapEntry::kRegExp,
                    names_->GetName(re->Pattern()));
  } else if (object->IsJSObject()) {
    const char* name = names_->GetName(
        GetConstructorName(JSObject::cast(object)));
    if (object->IsJSGlobalObject()) {
      const char* tag = objects_tags_.GetTag(object);
      if (tag != nullptr) {
        name = names_->GetFormatted("%s / %s", name, tag);
      }
    }
    return AddEntry(object, HeapEntry::kObject, name);
  } else if (object->IsString()) {
    String* string = String::cast(object);
    if (string->IsConsString())
      return AddEntry(object,
                      HeapEntry::kConsString,
                      "(concatenated string)");
    if (string->IsSlicedString())
      return AddEntry(object,
                      HeapEntry::kSlicedString,
                      "(sliced string)");
    return AddEntry(object,
                    HeapEntry::kString,
                    names_->GetName(String::cast(object)));
  } else if (object->IsSymbol()) {
    if (Symbol::cast(object)->is_private())
      return AddEntry(object, HeapEntry::kHidden, "private symbol");
    else
      return AddEntry(object, HeapEntry::kSymbol, "symbol");
  } else if (object->IsBigInt()) {
    return AddEntry(object, HeapEntry::kBigInt, "bigint");
  } else if (object->IsCode()) {
    return AddEntry(object, HeapEntry::kCode, "");
  } else if (object->IsSharedFunctionInfo()) {
    String* name = SharedFunctionInfo::cast(object)->Name();
    return AddEntry(object,
                    HeapEntry::kCode,
                    names_->GetName(name));
  } else if (object->IsScript()) {
    Object* name = Script::cast(object)->name();
    return AddEntry(object,
                    HeapEntry::kCode,
                    name->IsString()
                        ? names_->GetName(String::cast(name))
                        : "");
  } else if (object->IsNativeContext()) {
    return AddEntry(object, HeapEntry::kHidden, "system / NativeContext");
  } else if (object->IsContext()) {
    return AddEntry(object, HeapEntry::kObject, "system / Context");
  } else if (object->IsFixedArray() || object->IsFixedDoubleArray() ||
             object->IsByteArray()) {
    return AddEntry(object, HeapEntry::kArray, "");
  } else if (object->IsHeapNumber()) {
    return AddEntry(object, HeapEntry::kHeapNumber, "number");
  }
  return AddEntry(object, HeapEntry::kHidden, GetSystemEntryName(object));
}


HeapEntry* V8HeapExplorer::AddEntry(HeapObject* object,
                                    HeapEntry::Type type,
                                    const char* name) {
  return AddEntry(object->address(), type, name, object->Size());
}


HeapEntry* V8HeapExplorer::AddEntry(Address address,
                                    HeapEntry::Type type,
                                    const char* name,
                                    size_t size) {
  SnapshotObjectId object_id = heap_object_map_->FindOrAddEntry(
      address, static_cast<unsigned int>(size));
  unsigned trace_node_id = 0;
  if (AllocationTracker* allocation_tracker =
      snapshot_->profiler()->allocation_tracker()) {
    trace_node_id =
        allocation_tracker->address_to_trace()->GetTraceNodeId(address);
  }
  return snapshot_->AddEntry(type, name, object_id, size, trace_node_id);
}


class SnapshotFiller {
 public:
  explicit SnapshotFiller(HeapSnapshot* snapshot, HeapEntriesMap* entries)
      : snapshot_(snapshot),
        names_(snapshot->profiler()->names()),
        entries_(entries) { }
  HeapEntry* AddEntry(HeapThing ptr, HeapEntriesAllocator* allocator) {
    HeapEntry* entry = allocator->AllocateEntry(ptr);
    entries_->Pair(ptr, entry->index());
    return entry;
  }
  HeapEntry* FindEntry(HeapThing ptr) {
    int index = entries_->Map(ptr);
    return index != HeapEntry::kNoEntry ? &snapshot_->entries()[index]
                                        : nullptr;
  }
  HeapEntry* FindOrAddEntry(HeapThing ptr, HeapEntriesAllocator* allocator) {
    HeapEntry* entry = FindEntry(ptr);
    return entry != nullptr ? entry : AddEntry(ptr, allocator);
  }
  void SetIndexedReference(HeapGraphEdge::Type type,
                           int parent,
                           int index,
                           HeapEntry* child_entry) {
    HeapEntry* parent_entry = &snapshot_->entries()[parent];
    parent_entry->SetIndexedReference(type, index, child_entry);
  }
  void SetIndexedAutoIndexReference(HeapGraphEdge::Type type,
                                    int parent,
                                    HeapEntry* child_entry) {
    HeapEntry* parent_entry = &snapshot_->entries()[parent];
    int index = parent_entry->children_count() + 1;
    parent_entry->SetIndexedReference(type, index, child_entry);
  }
  void SetNamedReference(HeapGraphEdge::Type type,
                         int parent,
                         const char* reference_name,
                         HeapEntry* child_entry) {
    HeapEntry* parent_entry = &snapshot_->entries()[parent];
    parent_entry->SetNamedReference(type, reference_name, child_entry);
  }
  void SetNamedAutoIndexReference(HeapGraphEdge::Type type, int parent,
                                  const char* description,
                                  HeapEntry* child_entry) {
    HeapEntry* parent_entry = &snapshot_->entries()[parent];
    int index = parent_entry->children_count() + 1;
    const char* name = description
                           ? names_->GetFormatted("%d / %s", index, description)
                           : names_->GetName(index);
    parent_entry->SetNamedReference(type, name, child_entry);
  }

 private:
  HeapSnapshot* snapshot_;
  StringsStorage* names_;
  HeapEntriesMap* entries_;
};


const char* V8HeapExplorer::GetSystemEntryName(HeapObject* object) {
  switch (object->map()->instance_type()) {
    case MAP_TYPE:
      switch (Map::cast(object)->instance_type()) {
#define MAKE_STRING_MAP_CASE(instance_type, size, name, Name) \
        case instance_type: return "system / Map (" #Name ")";
      STRING_TYPE_LIST(MAKE_STRING_MAP_CASE)
#undef MAKE_STRING_MAP_CASE
        default: return "system / Map";
      }
    case CELL_TYPE: return "system / Cell";
    case PROPERTY_CELL_TYPE: return "system / PropertyCell";
    case FOREIGN_TYPE: return "system / Foreign";
    case ODDBALL_TYPE: return "system / Oddball";
    case ALLOCATION_SITE_TYPE:
      return "system / AllocationSite";
#define MAKE_STRUCT_CASE(NAME, Name, name) \
    case NAME##_TYPE: return "system / "#Name;
  STRUCT_LIST(MAKE_STRUCT_CASE)
#undef MAKE_STRUCT_CASE
    default: return "system";
  }
}

int V8HeapExplorer::EstimateObjectsCount() {
  HeapIterator it(heap_, HeapIterator::kFilterUnreachable);
  int objects_count = 0;
  while (it.next()) ++objects_count;
  return objects_count;
}

class IndexedReferencesExtractor : public ObjectVisitor {
 public:
  IndexedReferencesExtractor(V8HeapExplorer* generator, HeapObject* parent_obj,
                             int parent)
      : generator_(generator),
        parent_obj_(parent_obj),
        parent_start_(HeapObject::RawField(parent_obj_, 0)),
        parent_end_(HeapObject::RawField(parent_obj_, parent_obj_->Size())),
        parent_(parent) {}
  void VisitPointers(HeapObject* host, Object** start, Object** end) override {
    VisitPointers(host, reinterpret_cast<MaybeObject**>(start),
                  reinterpret_cast<MaybeObject**>(end));
  }
  void VisitPointers(HeapObject* host, MaybeObject** start,
                     MaybeObject** end) override {
    int next_index = 0;
    for (MaybeObject** p = start; p < end; p++) {
      int index = static_cast<int>(reinterpret_cast<Object**>(p) -
                                   HeapObject::RawField(parent_obj_, 0));
      ++next_index;
      // |p| could be outside of the object, e.g., while visiting RelocInfo of
      // code objects.
      if (reinterpret_cast<Object**>(p) >= parent_start_ &&
          reinterpret_cast<Object**>(p) < parent_end_ &&
          generator_->visited_fields_[index]) {
        generator_->visited_fields_[index] = false;
        continue;
      }
      HeapObject* heap_object;
      if ((*p)->ToWeakHeapObject(&heap_object) ||
          (*p)->ToStrongHeapObject(&heap_object)) {
        generator_->SetHiddenReference(parent_obj_, parent_, next_index,
                                       heap_object, index * kPointerSize);
      }
    }
  }

 private:
  V8HeapExplorer* generator_;
  HeapObject* parent_obj_;
  Object** parent_start_;
  Object** parent_end_;
  int parent_;
};

void V8HeapExplorer::ExtractReferences(int entry, HeapObject* obj) {
  if (obj->IsJSGlobalProxy()) {
    ExtractJSGlobalProxyReferences(entry, JSGlobalProxy::cast(obj));
  } else if (obj->IsJSArrayBuffer()) {
    ExtractJSArrayBufferReferences(entry, JSArrayBuffer::cast(obj));
  } else if (obj->IsJSObject()) {
    if (obj->IsJSWeakSet()) {
      ExtractJSWeakCollectionReferences(entry, JSWeakSet::cast(obj));
    } else if (obj->IsJSWeakMap()) {
      ExtractJSWeakCollectionReferences(entry, JSWeakMap::cast(obj));
    } else if (obj->IsJSSet()) {
      ExtractJSCollectionReferences(entry, JSSet::cast(obj));
    } else if (obj->IsJSMap()) {
      ExtractJSCollectionReferences(entry, JSMap::cast(obj));
    } else if (obj->IsJSPromise()) {
      ExtractJSPromiseReferences(entry, JSPromise::cast(obj));
    }
    ExtractJSObjectReferences(entry, JSObject::cast(obj));
  } else if (obj->IsString()) {
    ExtractStringReferences(entry, String::cast(obj));
  } else if (obj->IsSymbol()) {
    ExtractSymbolReferences(entry, Symbol::cast(obj));
  } else if (obj->IsMap()) {
    ExtractMapReferences(entry, Map::cast(obj));
  } else if (obj->IsSharedFunctionInfo()) {
    ExtractSharedFunctionInfoReferences(entry, SharedFunctionInfo::cast(obj));
  } else if (obj->IsScript()) {
    ExtractScriptReferences(entry, Script::cast(obj));
  } else if (obj->IsAccessorInfo()) {
    ExtractAccessorInfoReferences(entry, AccessorInfo::cast(obj));
  } else if (obj->IsAccessorPair()) {
    ExtractAccessorPairReferences(entry, AccessorPair::cast(obj));
  } else if (obj->IsCode()) {
    ExtractCodeReferences(entry, Code::cast(obj));
  } else if (obj->IsCell()) {
    ExtractCellReferences(entry, Cell::cast(obj));
  } else if (obj->IsFeedbackCell()) {
    ExtractFeedbackCellReferences(entry, FeedbackCell::cast(obj));
  } else if (obj->IsWeakCell()) {
    ExtractWeakCellReferences(entry, WeakCell::cast(obj));
  } else if (obj->IsPropertyCell()) {
    ExtractPropertyCellReferences(entry, PropertyCell::cast(obj));
  } else if (obj->IsAllocationSite()) {
    ExtractAllocationSiteReferences(entry, AllocationSite::cast(obj));
  } else if (obj->IsArrayBoilerplateDescription()) {
    ExtractArrayBoilerplateDescriptionReferences(
        entry, ArrayBoilerplateDescription::cast(obj));
  } else if (obj->IsFeedbackVector()) {
    ExtractFeedbackVectorReferences(entry, FeedbackVector::cast(obj));
  } else if (obj->IsWeakFixedArray()) {
    ExtractWeakArrayReferences(WeakFixedArray::kHeaderSize, entry,
                               WeakFixedArray::cast(obj));
  } else if (obj->IsWeakArrayList()) {
    ExtractWeakArrayReferences(WeakArrayList::kHeaderSize, entry,
                               WeakArrayList::cast(obj));
  } else if (obj->IsContext()) {
    ExtractContextReferences(entry, Context::cast(obj));
  } else if (obj->IsEphemeronHashTable()) {
    ExtractEphemeronHashTableReferences(entry, EphemeronHashTable::cast(obj));
  } else if (obj->IsFixedArray()) {
    ExtractFixedArrayReferences(entry, FixedArray::cast(obj));
  }
}


void V8HeapExplorer::ExtractJSGlobalProxyReferences(
    int entry, JSGlobalProxy* proxy) {
  SetInternalReference(proxy, entry,
                       "native_context", proxy->native_context(),
                       JSGlobalProxy::kNativeContextOffset);
}


void V8HeapExplorer::ExtractJSObjectReferences(
    int entry, JSObject* js_obj) {
  HeapObject* obj = js_obj;
  ExtractPropertyReferences(js_obj, entry);
  ExtractElementReferences(js_obj, entry);
  ExtractInternalReferences(js_obj, entry);
  PrototypeIterator iter(heap_->isolate(), js_obj);
  ReadOnlyRoots roots(heap_);
  SetPropertyReference(obj, entry, roots.proto_string(), iter.GetCurrent());
  if (obj->IsJSBoundFunction()) {
    JSBoundFunction* js_fun = JSBoundFunction::cast(obj);
    TagObject(js_fun->bound_arguments(), "(bound arguments)");
    SetInternalReference(js_fun, entry, "bindings", js_fun->bound_arguments(),
                         JSBoundFunction::kBoundArgumentsOffset);
    SetInternalReference(js_obj, entry, "bound_this", js_fun->bound_this(),
                         JSBoundFunction::kBoundThisOffset);
    SetInternalReference(js_obj, entry, "bound_function",
                         js_fun->bound_target_function(),
                         JSBoundFunction::kBoundTargetFunctionOffset);
    FixedArray* bindings = js_fun->bound_arguments();
    for (int i = 0; i < bindings->length(); i++) {
      const char* reference_name = names_->GetFormatted("bound_argument_%d", i);
      SetNativeBindReference(js_obj, entry, reference_name, bindings->get(i));
    }
  } else if (obj->IsJSFunction()) {
    JSFunction* js_fun = JSFunction::cast(js_obj);
    if (js_fun->has_prototype_slot()) {
      Object* proto_or_map = js_fun->prototype_or_initial_map();
      if (!proto_or_map->IsTheHole(heap_->isolate())) {
        if (!proto_or_map->IsMap()) {
          SetPropertyReference(obj, entry, roots.prototype_string(),
                               proto_or_map, nullptr,
                               JSFunction::kPrototypeOrInitialMapOffset);
        } else {
          SetPropertyReference(obj, entry, roots.prototype_string(),
                               js_fun->prototype());
          SetInternalReference(obj, entry, "initial_map", proto_or_map,
                               JSFunction::kPrototypeOrInitialMapOffset);
        }
      }
    }
    SharedFunctionInfo* shared_info = js_fun->shared();
    TagObject(js_fun->feedback_cell(), "(function feedback cell)");
    SetInternalReference(js_fun, entry, "feedback_cell",
                         js_fun->feedback_cell(),
                         JSFunction::kFeedbackCellOffset);
    TagObject(shared_info, "(shared function info)");
    SetInternalReference(js_fun, entry,
                         "shared", shared_info,
                         JSFunction::kSharedFunctionInfoOffset);
    TagObject(js_fun->context(), "(context)");
    SetInternalReference(js_fun, entry,
                         "context", js_fun->context(),
                         JSFunction::kContextOffset);
    TagCodeObject(js_fun->code());
    SetInternalReference(js_fun, entry, "code", js_fun->code(),
                         JSFunction::kCodeOffset);
  } else if (obj->IsJSGlobalObject()) {
    JSGlobalObject* global_obj = JSGlobalObject::cast(obj);
    SetInternalReference(global_obj, entry, "native_context",
                         global_obj->native_context(),
                         JSGlobalObject::kNativeContextOffset);
    SetInternalReference(global_obj, entry, "global_proxy",
                         global_obj->global_proxy(),
                         JSGlobalObject::kGlobalProxyOffset);
    STATIC_ASSERT(JSGlobalObject::kSize - JSObject::kHeaderSize ==
                  2 * kPointerSize);
  } else if (obj->IsJSArrayBufferView()) {
    JSArrayBufferView* view = JSArrayBufferView::cast(obj);
    SetInternalReference(view, entry, "buffer", view->buffer(),
                         JSArrayBufferView::kBufferOffset);
  }

  TagObject(js_obj->raw_properties_or_hash(), "(object properties)");
  SetInternalReference(obj, entry, "properties",
                       js_obj->raw_properties_or_hash(),
                       JSObject::kPropertiesOrHashOffset);

  TagObject(js_obj->elements(), "(object elements)");
  SetInternalReference(obj, entry,
                       "elements", js_obj->elements(),
                       JSObject::kElementsOffset);
}


void V8HeapExplorer::ExtractStringReferences(int entry, String* string) {
  if (string->IsConsString()) {
    ConsString* cs = ConsString::cast(string);
    SetInternalReference(cs, entry, "first", cs->first(),
                         ConsString::kFirstOffset);
    SetInternalReference(cs, entry, "second", cs->second(),
                         ConsString::kSecondOffset);
  } else if (string->IsSlicedString()) {
    SlicedString* ss = SlicedString::cast(string);
    SetInternalReference(ss, entry, "parent", ss->parent(),
                         SlicedString::kParentOffset);
  } else if (string->IsThinString()) {
    ThinString* ts = ThinString::cast(string);
    SetInternalReference(ts, entry, "actual", ts->actual(),
                         ThinString::kActualOffset);
  }
}


void V8HeapExplorer::ExtractSymbolReferences(int entry, Symbol* symbol) {
  SetInternalReference(symbol, entry,
                       "name", symbol->name(),
                       Symbol::kNameOffset);
}


void V8HeapExplorer::ExtractJSCollectionReferences(int entry,
                                                   JSCollection* collection) {
  SetInternalReference(collection, entry, "table", collection->table(),
                       JSCollection::kTableOffset);
}

void V8HeapExplorer::ExtractJSWeakCollectionReferences(int entry,
                                                       JSWeakCollection* obj) {
  SetInternalReference(obj, entry, "table", obj->table(),
                       JSWeakCollection::kTableOffset);
}

void V8HeapExplorer::ExtractEphemeronHashTableReferences(
    int entry, EphemeronHashTable* table) {
  for (int i = 0, capacity = table->Capacity(); i < capacity; ++i) {
    int key_index = EphemeronHashTable::EntryToIndex(i) +
                    EphemeronHashTable::kEntryKeyIndex;
    int value_index = EphemeronHashTable::EntryToValueIndex(i);
    Object* key = table->get(key_index);
    Object* value = table->get(value_index);
    SetWeakReference(table, entry, key_index, key,
                     table->OffsetOfElementAt(key_index));
    SetInternalReference(table, entry, value_index, value,
                         table->OffsetOfElementAt(value_index));
    HeapEntry* key_entry = GetEntry(key);
    int key_entry_index = key_entry->index();
    HeapEntry* value_entry = GetEntry(value);
    if (key_entry && value_entry) {
      const char* edge_name =
          names_->GetFormatted("key %s in WeakMap", key_entry->name());
      filler_->SetNamedAutoIndexReference(
          HeapGraphEdge::kInternal, key_entry_index, edge_name, value_entry);
    }
  }
}

void V8HeapExplorer::ExtractContextReferences(int entry, Context* context) {
  if (!context->IsNativeContext() && context->is_declaration_context()) {
    ScopeInfo* scope_info = context->scope_info();
    // Add context allocated locals.
    int context_locals = scope_info->ContextLocalCount();
    for (int i = 0; i < context_locals; ++i) {
      String* local_name = scope_info->ContextLocalName(i);
      int idx = Context::MIN_CONTEXT_SLOTS + i;
      SetContextReference(context, entry, local_name, context->get(idx),
                          Context::OffsetOfElementAt(idx));
    }
    if (scope_info->HasFunctionName()) {
      String* name = String::cast(scope_info->FunctionName());
      int idx = scope_info->FunctionContextSlotIndex(name);
      if (idx >= 0) {
        SetContextReference(context, entry, name, context->get(idx),
                            Context::OffsetOfElementAt(idx));
      }
    }
  }

#define EXTRACT_CONTEXT_FIELD(index, type, name) \
  if (Context::index < Context::FIRST_WEAK_SLOT || \
      Context::index == Context::MAP_CACHE_INDEX) { \
    SetInternalReference(context, entry, #name, context->get(Context::index), \
        FixedArray::OffsetOfElementAt(Context::index)); \
  } else { \
    SetWeakReference(context, entry, #name, context->get(Context::index), \
        FixedArray::OffsetOfElementAt(Context::index)); \
  }
  EXTRACT_CONTEXT_FIELD(SCOPE_INFO_INDEX, ScopeInfo, scope_info);
  EXTRACT_CONTEXT_FIELD(PREVIOUS_INDEX, Context, previous);
  EXTRACT_CONTEXT_FIELD(EXTENSION_INDEX, HeapObject, extension);
  EXTRACT_CONTEXT_FIELD(NATIVE_CONTEXT_INDEX, Context, native_context);
  if (context->IsNativeContext()) {
    TagObject(context->normalized_map_cache(), "(context norm. map cache)");
    TagObject(context->embedder_data(), "(context data)");
    NATIVE_CONTEXT_FIELDS(EXTRACT_CONTEXT_FIELD)
    EXTRACT_CONTEXT_FIELD(OPTIMIZED_CODE_LIST, unused, optimized_code_list);
    EXTRACT_CONTEXT_FIELD(DEOPTIMIZED_CODE_LIST, unused, deoptimized_code_list);
#undef EXTRACT_CONTEXT_FIELD
    STATIC_ASSERT(Context::OPTIMIZED_CODE_LIST == Context::FIRST_WEAK_SLOT);
    STATIC_ASSERT(Context::NEXT_CONTEXT_LINK + 1 ==
                  Context::NATIVE_CONTEXT_SLOTS);
    STATIC_ASSERT(Context::FIRST_WEAK_SLOT + 3 ==
                  Context::NATIVE_CONTEXT_SLOTS);
  }
}


void V8HeapExplorer::ExtractMapReferences(int entry, Map* map) {
  MaybeObject* maybe_raw_transitions_or_prototype_info = map->raw_transitions();
  HeapObject* raw_transitions_or_prototype_info;
  if (maybe_raw_transitions_or_prototype_info->ToWeakHeapObject(
          &raw_transitions_or_prototype_info)) {
    DCHECK(raw_transitions_or_prototype_info->IsMap());
    SetWeakReference(map, entry, "transition",
                     raw_transitions_or_prototype_info,
                     Map::kTransitionsOrPrototypeInfoOffset);
  } else if (maybe_raw_transitions_or_prototype_info->ToStrongHeapObject(
                 &raw_transitions_or_prototype_info)) {
    DCHECK(!raw_transitions_or_prototype_info->IsWeakCell());

    if (raw_transitions_or_prototype_info->IsTransitionArray()) {
      TransitionArray* transitions =
          TransitionArray::cast(raw_transitions_or_prototype_info);
      if (map->CanTransition() && transitions->HasPrototypeTransitions()) {
        TagObject(transitions->GetPrototypeTransitions(),
                  "(prototype transitions)");
      }
      TagObject(transitions, "(transition array)");
      SetInternalReference(map, entry, "transitions", transitions,
                           Map::kTransitionsOrPrototypeInfoOffset);
    } else if (raw_transitions_or_prototype_info->IsTuple3() ||
               raw_transitions_or_prototype_info->IsFixedArray()) {
      TagObject(raw_transitions_or_prototype_info, "(transition)");
      SetInternalReference(map, entry, "transition",
                           raw_transitions_or_prototype_info,
                           Map::kTransitionsOrPrototypeInfoOffset);
    } else if (map->is_prototype_map()) {
      TagObject(raw_transitions_or_prototype_info, "prototype_info");
      SetInternalReference(map, entry, "prototype_info",
                           raw_transitions_or_prototype_info,
                           Map::kTransitionsOrPrototypeInfoOffset);
    }
  }
  DescriptorArray* descriptors = map->instance_descriptors();
  TagObject(descriptors, "(map descriptors)");
  SetInternalReference(map, entry, "descriptors", descriptors,
                       Map::kDescriptorsOffset);
  SetInternalReference(map, entry, "prototype", map->prototype(),
                       Map::kPrototypeOffset);
  if (FLAG_unbox_double_fields) {
    SetInternalReference(map, entry, "layout_descriptor",
                         map->layout_descriptor(),
                         Map::kLayoutDescriptorOffset);
  }
  Object* constructor_or_backpointer = map->constructor_or_backpointer();
  if (constructor_or_backpointer->IsMap()) {
    TagObject(constructor_or_backpointer, "(back pointer)");
    SetInternalReference(map, entry, "back_pointer", constructor_or_backpointer,
                         Map::kConstructorOrBackPointerOffset);
  } else if (constructor_or_backpointer->IsFunctionTemplateInfo()) {
    TagObject(constructor_or_backpointer, "(constructor function data)");
    SetInternalReference(map, entry, "constructor_function_data",
                         constructor_or_backpointer,
                         Map::kConstructorOrBackPointerOffset);
  } else {
    SetInternalReference(map, entry, "constructor", constructor_or_backpointer,
                         Map::kConstructorOrBackPointerOffset);
  }
  TagObject(map->dependent_code(), "(dependent code)");
  SetInternalReference(map, entry, "dependent_code", map->dependent_code(),
                       Map::kDependentCodeOffset);
}


void V8HeapExplorer::ExtractSharedFunctionInfoReferences(
    int entry, SharedFunctionInfo* shared) {
  HeapObject* obj = shared;
  String* shared_name = shared->DebugName();
  const char* name = nullptr;
  if (shared_name != ReadOnlyRoots(heap_).empty_string()) {
    name = names_->GetName(shared_name);
    TagObject(shared->GetCode(), names_->GetFormatted("(code for %s)", name));
  } else {
    TagObject(shared->GetCode(),
              names_->GetFormatted(
                  "(%s code)", Code::Kind2String(shared->GetCode()->kind())));
  }

  if (shared->name_or_scope_info()->IsScopeInfo()) {
    TagObject(shared->name_or_scope_info(), "(function scope info)");
  }
  SetInternalReference(obj, entry, "name_or_scope_info",
                       shared->name_or_scope_info(),
                       SharedFunctionInfo::kNameOrScopeInfoOffset);
  SetInternalReference(obj, entry, "script_or_debug_info",
                       shared->script_or_debug_info(),
                       SharedFunctionInfo::kScriptOrDebugInfoOffset);
  SetInternalReference(obj, entry,
                       "function_data", shared->function_data(),
                       SharedFunctionInfo::kFunctionDataOffset);
  SetInternalReference(
      obj, entry, "raw_outer_scope_info_or_feedback_metadata",
      shared->raw_outer_scope_info_or_feedback_metadata(),
      SharedFunctionInfo::kOuterScopeInfoOrFeedbackMetadataOffset);
}

void V8HeapExplorer::ExtractScriptReferences(int entry, Script* script) {
  HeapObject* obj = script;
  SetInternalReference(obj, entry,
                       "source", script->source(),
                       Script::kSourceOffset);
  SetInternalReference(obj, entry,
                       "name", script->name(),
                       Script::kNameOffset);
  SetInternalReference(obj, entry,
                       "context_data", script->context_data(),
                       Script::kContextOffset);
  TagObject(script->line_ends(), "(script line ends)");
  SetInternalReference(obj, entry,
                       "line_ends", script->line_ends(),
                       Script::kLineEndsOffset);
}


void V8HeapExplorer::ExtractAccessorInfoReferences(
    int entry, AccessorInfo* accessor_info) {
  SetInternalReference(accessor_info, entry, "name", accessor_info->name(),
                       AccessorInfo::kNameOffset);
  SetInternalReference(accessor_info, entry, "expected_receiver_type",
                       accessor_info->expected_receiver_type(),
                       AccessorInfo::kExpectedReceiverTypeOffset);
  SetInternalReference(accessor_info, entry, "getter", accessor_info->getter(),
                       AccessorInfo::kGetterOffset);
  SetInternalReference(accessor_info, entry, "setter", accessor_info->setter(),
                       AccessorInfo::kSetterOffset);
  SetInternalReference(accessor_info, entry, "data", accessor_info->data(),
                       AccessorInfo::kDataOffset);
}

void V8HeapExplorer::ExtractAccessorPairReferences(
    int entry, AccessorPair* accessors) {
  SetInternalReference(accessors, entry, "getter", accessors->getter(),
                       AccessorPair::kGetterOffset);
  SetInternalReference(accessors, entry, "setter", accessors->setter(),
                       AccessorPair::kSetterOffset);
}

void V8HeapExplorer::TagBuiltinCodeObject(Code* code, const char* name) {
  TagObject(code, names_->GetFormatted("(%s builtin)", name));
}

void V8HeapExplorer::TagCodeObject(Code* code) {
  if (code->kind() == Code::STUB) {
    TagObject(code, names_->GetFormatted(
                        "(%s code)",
                        CodeStub::MajorName(CodeStub::GetMajorKey(code))));
  }
}

void V8HeapExplorer::ExtractCodeReferences(int entry, Code* code) {
  TagCodeObject(code);
  TagObject(code->relocation_info(), "(code relocation info)");
  SetInternalReference(code, entry,
                       "relocation_info", code->relocation_info(),
                       Code::kRelocationInfoOffset);
  TagObject(code->deoptimization_data(), "(code deopt data)");
  SetInternalReference(code, entry,
                       "deoptimization_data", code->deoptimization_data(),
                       Code::kDeoptimizationDataOffset);
  TagObject(code->source_position_table(), "(source position table)");
  SetInternalReference(code, entry, "source_position_table",
                       code->source_position_table(),
                       Code::kSourcePositionTableOffset);
}

void V8HeapExplorer::ExtractCellReferences(int entry, Cell* cell) {
  SetInternalReference(cell, entry, "value", cell->value(), Cell::kValueOffset);
}

void V8HeapExplorer::ExtractFeedbackCellReferences(
    int entry, FeedbackCell* feedback_cell) {
  TagObject(feedback_cell, "(feedback cell)");
  SetInternalReference(feedback_cell, entry, "value", feedback_cell->value(),
                       FeedbackCell::kValueOffset);
}

void V8HeapExplorer::ExtractWeakCellReferences(int entry, WeakCell* weak_cell) {
  TagObject(weak_cell, "(weak cell)");
  SetWeakReference(weak_cell, entry, "value", weak_cell->value(),
                   WeakCell::kValueOffset);
}

void V8HeapExplorer::ExtractPropertyCellReferences(int entry,
                                                   PropertyCell* cell) {
  SetInternalReference(cell, entry, "value", cell->value(),
                       PropertyCell::kValueOffset);
  TagObject(cell->dependent_code(), "(dependent code)");
  SetInternalReference(cell, entry, "dependent_code", cell->dependent_code(),
                       PropertyCell::kDependentCodeOffset);
}

void V8HeapExplorer::ExtractAllocationSiteReferences(int entry,
                                                     AllocationSite* site) {
  SetInternalReference(site, entry, "transition_info",
                       site->transition_info_or_boilerplate(),
                       AllocationSite::kTransitionInfoOrBoilerplateOffset);
  SetInternalReference(site, entry, "nested_site", site->nested_site(),
                       AllocationSite::kNestedSiteOffset);
  TagObject(site->dependent_code(), "(dependent code)");
  SetInternalReference(site, entry, "dependent_code", site->dependent_code(),
                       AllocationSite::kDependentCodeOffset);
}

void V8HeapExplorer::ExtractArrayBoilerplateDescriptionReferences(
    int entry, ArrayBoilerplateDescription* value) {
  SetInternalReference(value, entry, "constant_elements",
                       value->constant_elements(),
                       ArrayBoilerplateDescription::kConstantElementsOffset);
}

class JSArrayBufferDataEntryAllocator : public HeapEntriesAllocator {
 public:
  JSArrayBufferDataEntryAllocator(size_t size, V8HeapExplorer* explorer)
      : size_(size)
      , explorer_(explorer) {
  }
  virtual HeapEntry* AllocateEntry(HeapThing ptr) {
    return explorer_->AddEntry(reinterpret_cast<Address>(ptr),
                               HeapEntry::kNative, "system / JSArrayBufferData",
                               size_);
  }
 private:
  size_t size_;
  V8HeapExplorer* explorer_;
};

void V8HeapExplorer::ExtractJSArrayBufferReferences(
    int entry, JSArrayBuffer* buffer) {
  // Setup a reference to a native memory backing_store object.
  if (!buffer->backing_store())
    return;
  size_t data_size = NumberToSize(buffer->byte_length());
  JSArrayBufferDataEntryAllocator allocator(data_size, this);
  HeapEntry* data_entry =
      filler_->FindOrAddEntry(buffer->backing_store(), &allocator);
  filler_->SetNamedReference(HeapGraphEdge::kInternal,
                             entry, "backing_store", data_entry);
}

void V8HeapExplorer::ExtractJSPromiseReferences(int entry, JSPromise* promise) {
  SetInternalReference(promise, entry, "reactions_or_result",
                       promise->reactions_or_result(),
                       JSPromise::kReactionsOrResultOffset);
}

void V8HeapExplorer::ExtractFixedArrayReferences(int entry, FixedArray* array) {
  for (int i = 0, l = array->length(); i < l; ++i) {
    DCHECK(!HasWeakHeapObjectTag(array->get(i)));
    SetInternalReference(array, entry, i, array->get(i),
                         array->OffsetOfElementAt(i));
  }
}

void V8HeapExplorer::ExtractFeedbackVectorReferences(
    int entry, FeedbackVector* feedback_vector) {
  MaybeObject* code = feedback_vector->optimized_code_weak_or_smi();
  HeapObject* code_heap_object;
  if (code->ToWeakHeapObject(&code_heap_object)) {
    SetWeakReference(feedback_vector, entry, "optimized code", code_heap_object,
                     FeedbackVector::kOptimizedCodeOffset);
  }
}

template <typename T>
void V8HeapExplorer::ExtractWeakArrayReferences(int header_size, int entry,
                                                T* array) {
  for (int i = 0; i < array->length(); ++i) {
    MaybeObject* object = array->Get(i);
    HeapObject* heap_object;
    if (object->ToWeakHeapObject(&heap_object)) {
      SetWeakReference(array, entry, i, heap_object,
                       header_size + i * kPointerSize);
    } else if (object->ToStrongHeapObject(&heap_object)) {
      SetInternalReference(array, entry, i, heap_object,
                           header_size + i * kPointerSize);
    }
  }
}

void V8HeapExplorer::ExtractPropertyReferences(JSObject* js_obj, int entry) {
  Isolate* isolate = js_obj->GetIsolate();
  if (js_obj->HasFastProperties()) {
    DescriptorArray* descs = js_obj->map()->instance_descriptors();
    int real_size = js_obj->map()->NumberOfOwnDescriptors();
    for (int i = 0; i < real_size; i++) {
      PropertyDetails details = descs->GetDetails(i);
      switch (details.location()) {
        case kField: {
          Representation r = details.representation();
          if (r.IsSmi() || r.IsDouble()) break;

          Name* k = descs->GetKey(i);
          FieldIndex field_index = FieldIndex::ForDescriptor(js_obj->map(), i);
          Object* value = js_obj->RawFastPropertyAt(field_index);
          int field_offset =
              field_index.is_inobject() ? field_index.offset() : -1;

          SetDataOrAccessorPropertyReference(details.kind(), js_obj, entry, k,
                                             value, nullptr, field_offset);
          break;
        }
        case kDescriptor:
          SetDataOrAccessorPropertyReference(details.kind(), js_obj, entry,
                                             descs->GetKey(i),
                                             descs->GetStrongValue(i));
          break;
      }
    }
  } else if (js_obj->IsJSGlobalObject()) {
    // We assume that global objects can only have slow properties.
    GlobalDictionary* dictionary =
        JSGlobalObject::cast(js_obj)->global_dictionary();
    int length = dictionary->Capacity();
    ReadOnlyRoots roots(isolate);
    for (int i = 0; i < length; ++i) {
      if (dictionary->IsKey(roots, dictionary->KeyAt(i))) {
        PropertyCell* cell = dictionary->CellAt(i);
        Name* name = cell->name();
        Object* value = cell->value();
        PropertyDetails details = cell->property_details();
        SetDataOrAccessorPropertyReference(details.kind(), js_obj, entry, name,
                                           value);
      }
    }
  } else {
    NameDictionary* dictionary = js_obj->property_dictionary();
    int length = dictionary->Capacity();
    ReadOnlyRoots roots(isolate);
    for (int i = 0; i < length; ++i) {
      Object* k = dictionary->KeyAt(i);
      if (dictionary->IsKey(roots, k)) {
        Object* value = dictionary->ValueAt(i);
        PropertyDetails details = dictionary->DetailsAt(i);
        SetDataOrAccessorPropertyReference(details.kind(), js_obj, entry,
                                           Name::cast(k), value);
      }
    }
  }
}


void V8HeapExplorer::ExtractAccessorPairProperty(JSObject* js_obj, int entry,
                                                 Name* key,
                                                 Object* callback_obj,
                                                 int field_offset) {
  if (!callback_obj->IsAccessorPair()) return;
  AccessorPair* accessors = AccessorPair::cast(callback_obj);
  SetPropertyReference(js_obj, entry, key, accessors, nullptr, field_offset);
  Object* getter = accessors->getter();
  if (!getter->IsOddball()) {
    SetPropertyReference(js_obj, entry, key, getter, "get %s");
  }
  Object* setter = accessors->setter();
  if (!setter->IsOddball()) {
    SetPropertyReference(js_obj, entry, key, setter, "set %s");
  }
}


void V8HeapExplorer::ExtractElementReferences(JSObject* js_obj, int entry) {
  ReadOnlyRoots roots = js_obj->GetReadOnlyRoots();
  if (js_obj->HasObjectElements()) {
    FixedArray* elements = FixedArray::cast(js_obj->elements());
    int length = js_obj->IsJSArray()
                     ? Smi::ToInt(JSArray::cast(js_obj)->length())
                     : elements->length();
    for (int i = 0; i < length; ++i) {
      if (!elements->get(i)->IsTheHole(roots)) {
        SetElementReference(js_obj, entry, i, elements->get(i));
      }
    }
  } else if (js_obj->HasDictionaryElements()) {
    NumberDictionary* dictionary = js_obj->element_dictionary();
    int length = dictionary->Capacity();
    for (int i = 0; i < length; ++i) {
      Object* k = dictionary->KeyAt(i);
      if (dictionary->IsKey(roots, k)) {
        DCHECK(k->IsNumber());
        uint32_t index = static_cast<uint32_t>(k->Number());
        SetElementReference(js_obj, entry, index, dictionary->ValueAt(i));
      }
    }
  }
}


void V8HeapExplorer::ExtractInternalReferences(JSObject* js_obj, int entry) {
  int length = js_obj->GetEmbedderFieldCount();
  for (int i = 0; i < length; ++i) {
    Object* o = js_obj->GetEmbedderField(i);
    SetInternalReference(js_obj, entry, i, o,
                         js_obj->GetEmbedderFieldOffset(i));
  }
}


String* V8HeapExplorer::GetConstructorName(JSObject* object) {
  Isolate* isolate = object->GetIsolate();
  if (object->IsJSFunction()) return ReadOnlyRoots(isolate).closure_string();
  DisallowHeapAllocation no_gc;
  HandleScope scope(isolate);
  return *JSReceiver::GetConstructorName(handle(object, isolate));
}


HeapEntry* V8HeapExplorer::GetEntry(Object* obj) {
  if (!obj->IsHeapObject()) return nullptr;
  return filler_->FindOrAddEntry(obj, this);
}

class RootsReferencesExtractor : public RootVisitor {
 public:
  explicit RootsReferencesExtractor(V8HeapExplorer* explorer)
      : explorer_(explorer), visiting_weak_roots_(false) {}

  void SetVisitingWeakRoots() { visiting_weak_roots_ = true; }

  void VisitRootPointer(Root root, const char* description,
                        Object** object) override {
    if (root == Root::kBuiltins) {
      explorer_->TagBuiltinCodeObject(Code::cast(*object), description);
    }
    explorer_->SetGcSubrootReference(root, description, visiting_weak_roots_,
                                     *object);
  }

  void VisitRootPointers(Root root, const char* description, Object** start,
                         Object** end) override {
    for (Object** p = start; p < end; p++)
      VisitRootPointer(root, description, p);
  }

 private:
  V8HeapExplorer* explorer_;
  bool visiting_weak_roots_;
};

bool V8HeapExplorer::IterateAndExtractReferences(SnapshotFiller* filler) {
  filler_ = filler;

  // Create references to the synthetic roots.
  SetRootGcRootsReference();
  for (int root = 0; root < static_cast<int>(Root::kNumberOfRoots); root++) {
    SetGcRootsReference(static_cast<Root>(root));
  }

  // Make sure builtin code objects get their builtin tags
  // first. Otherwise a particular JSFunction object could set
  // its custom name to a generic builtin.
  RootsReferencesExtractor extractor(this);
  heap_->IterateRoots(&extractor, VISIT_ONLY_STRONG);
  extractor.SetVisitingWeakRoots();
  heap_->IterateWeakGlobalHandles(&extractor);

  bool interrupted = false;

  HeapIterator iterator(heap_, HeapIterator::kFilterUnreachable);
  // Heap iteration with filtering must be finished in any case.
  for (HeapObject *obj = iterator.next(); obj != nullptr;
       obj = iterator.next(), progress_->ProgressStep()) {
    if (interrupted) continue;

    size_t max_pointer = obj->Size() / kPointerSize;
    if (max_pointer > visited_fields_.size()) {
      // Clear the current bits.
      std::vector<bool>().swap(visited_fields_);
      // Reallocate to right size.
      visited_fields_.resize(max_pointer, false);
    }

    HeapEntry* heap_entry = GetEntry(obj);
    int entry = heap_entry->index();
    ExtractReferences(entry, obj);
    SetInternalReference(obj, entry, "map", obj->map(), HeapObject::kMapOffset);
    // Extract unvisited fields as hidden references and restore tags
    // of visited fields.
    IndexedReferencesExtractor refs_extractor(this, obj, entry);
    obj->Iterate(&refs_extractor);

    // Ensure visited_fields_ doesn't leak to the next object.
    for (size_t i = 0; i < max_pointer; ++i) {
      DCHECK(!visited_fields_[i]);
    }

    if (!progress_->ProgressReport(false)) interrupted = true;
  }

  filler_ = nullptr;
  return interrupted ? false : progress_->ProgressReport(true);
}


bool V8HeapExplorer::IsEssentialObject(Object* object) {
  ReadOnlyRoots roots(heap_);
  return object->IsHeapObject() && !object->IsOddball() &&
         object != roots.empty_byte_array() &&
         object != roots.empty_fixed_array() &&
         object != roots.empty_weak_fixed_array() &&
         object != roots.empty_descriptor_array() &&
         object != roots.fixed_array_map() && object != roots.cell_map() &&
         object != roots.global_property_cell_map() &&
         object != roots.shared_function_info_map() &&
         object != roots.free_space_map() &&
         object != roots.one_pointer_filler_map() &&
         object != roots.two_pointer_filler_map();
}

bool V8HeapExplorer::IsEssentialHiddenReference(Object* parent,
                                                int field_offset) {
  if (parent->IsAllocationSite() &&
      field_offset == AllocationSite::kWeakNextOffset)
    return false;
  if (parent->IsCodeDataContainer() &&
      field_offset == CodeDataContainer::kNextCodeLinkOffset)
    return false;
  if (parent->IsContext() &&
      field_offset == Context::OffsetOfElementAt(Context::NEXT_CONTEXT_LINK))
    return false;
  return true;
}

void V8HeapExplorer::SetContextReference(HeapObject* parent_obj,
                                         int parent_entry,
                                         String* reference_name,
                                         Object* child_obj,
                                         int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  filler_->SetNamedReference(HeapGraphEdge::kContextVariable, parent_entry,
                             names_->GetName(reference_name), child_entry);
  MarkVisitedField(field_offset);
}

void V8HeapExplorer::MarkVisitedField(int offset) {
  if (offset < 0) return;
  int index = offset / kPointerSize;
  DCHECK(!visited_fields_[index]);
  visited_fields_[index] = true;
}


void V8HeapExplorer::SetNativeBindReference(HeapObject* parent_obj,
                                            int parent_entry,
                                            const char* reference_name,
                                            Object* child_obj) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  filler_->SetNamedReference(HeapGraphEdge::kShortcut, parent_entry,
                             reference_name, child_entry);
}


void V8HeapExplorer::SetElementReference(HeapObject* parent_obj,
                                         int parent_entry,
                                         int index,
                                         Object* child_obj) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  filler_->SetIndexedReference(HeapGraphEdge::kElement, parent_entry, index,
                               child_entry);
}


void V8HeapExplorer::SetInternalReference(HeapObject* parent_obj,
                                          int parent_entry,
                                          const char* reference_name,
                                          Object* child_obj,
                                          int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  if (IsEssentialObject(child_obj)) {
    filler_->SetNamedReference(HeapGraphEdge::kInternal,
                               parent_entry,
                               reference_name,
                               child_entry);
  }
  MarkVisitedField(field_offset);
}


void V8HeapExplorer::SetInternalReference(HeapObject* parent_obj,
                                          int parent_entry,
                                          int index,
                                          Object* child_obj,
                                          int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  if (IsEssentialObject(child_obj)) {
    filler_->SetNamedReference(HeapGraphEdge::kInternal,
                               parent_entry,
                               names_->GetName(index),
                               child_entry);
  }
  MarkVisitedField(field_offset);
}

void V8HeapExplorer::SetHiddenReference(HeapObject* parent_obj,
                                        int parent_entry, int index,
                                        Object* child_obj, int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry != nullptr && IsEssentialObject(child_obj) &&
      IsEssentialHiddenReference(parent_obj, field_offset)) {
    filler_->SetIndexedReference(HeapGraphEdge::kHidden, parent_entry, index,
                                 child_entry);
  }
}


void V8HeapExplorer::SetWeakReference(HeapObject* parent_obj,
                                      int parent_entry,
                                      const char* reference_name,
                                      Object* child_obj,
                                      int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  if (IsEssentialObject(child_obj)) {
    filler_->SetNamedReference(HeapGraphEdge::kWeak,
                               parent_entry,
                               reference_name,
                               child_entry);
  }
  MarkVisitedField(field_offset);
}


void V8HeapExplorer::SetWeakReference(HeapObject* parent_obj,
                                      int parent_entry,
                                      int index,
                                      Object* child_obj,
                                      int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  if (IsEssentialObject(child_obj)) {
    filler_->SetNamedReference(HeapGraphEdge::kWeak,
                               parent_entry,
                               names_->GetFormatted("%d", index),
                               child_entry);
  }
  MarkVisitedField(field_offset);
}

void V8HeapExplorer::SetDataOrAccessorPropertyReference(
    PropertyKind kind, JSObject* parent_obj, int parent_entry,
    Name* reference_name, Object* child_obj, const char* name_format_string,
    int field_offset) {
  if (kind == kAccessor) {
    ExtractAccessorPairProperty(parent_obj, parent_entry, reference_name,
                                child_obj, field_offset);
  } else {
    SetPropertyReference(parent_obj, parent_entry, reference_name, child_obj,
                         name_format_string, field_offset);
  }
}


void V8HeapExplorer::SetPropertyReference(HeapObject* parent_obj,
                                          int parent_entry,
                                          Name* reference_name,
                                          Object* child_obj,
                                          const char* name_format_string,
                                          int field_offset) {
  DCHECK(parent_entry == GetEntry(parent_obj)->index());
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  HeapGraphEdge::Type type =
      reference_name->IsSymbol() || String::cast(reference_name)->length() > 0
          ? HeapGraphEdge::kProperty
          : HeapGraphEdge::kInternal;
  const char* name =
      name_format_string != nullptr && reference_name->IsString()
          ? names_->GetFormatted(
                name_format_string,
                String::cast(reference_name)
                    ->ToCString(DISALLOW_NULLS, ROBUST_STRING_TRAVERSAL)
                    .get())
          : names_->GetName(reference_name);

  filler_->SetNamedReference(type, parent_entry, name, child_entry);
  MarkVisitedField(field_offset);
}

void V8HeapExplorer::SetRootGcRootsReference() {
  filler_->SetIndexedAutoIndexReference(
      HeapGraphEdge::kElement,
      snapshot_->root()->index(),
      snapshot_->gc_roots());
}

void V8HeapExplorer::SetUserGlobalReference(Object* child_obj) {
  HeapEntry* child_entry = GetEntry(child_obj);
  DCHECK_NOT_NULL(child_entry);
  filler_->SetNamedAutoIndexReference(HeapGraphEdge::kShortcut,
                                      snapshot_->root()->index(), nullptr,
                                      child_entry);
}

void V8HeapExplorer::SetGcRootsReference(Root root) {
  filler_->SetIndexedAutoIndexReference(HeapGraphEdge::kElement,
                                        snapshot_->gc_roots()->index(),
                                        snapshot_->gc_subroot(root));
}

void V8HeapExplorer::SetGcSubrootReference(Root root, const char* description,
                                           bool is_weak, Object* child_obj) {
  HeapEntry* child_entry = GetEntry(child_obj);
  if (child_entry == nullptr) return;
  const char* name = GetStrongGcSubrootName(child_obj);
  HeapGraphEdge::Type edge_type =
      is_weak ? HeapGraphEdge::kWeak : HeapGraphEdge::kInternal;
  if (name != nullptr) {
    filler_->SetNamedReference(edge_type, snapshot_->gc_subroot(root)->index(),
                               name, child_entry);
  } else {
    filler_->SetNamedAutoIndexReference(edge_type,
                                        snapshot_->gc_subroot(root)->index(),
                                        description, child_entry);
  }

  // Add a shortcut to JS global object reference at snapshot root.
  // That allows the user to easily find global objects. They are
  // also used as starting points in distance calculations.
  if (is_weak || !child_obj->IsNativeContext()) return;

  JSGlobalObject* global = Context::cast(child_obj)->global_object();
  if (!global->IsJSGlobalObject()) return;

  if (user_roots_.Contains(global)) return;

  user_roots_.Insert(global);
  SetUserGlobalReference(global);
}

const char* V8HeapExplorer::GetStrongGcSubrootName(Object* object) {
  ReadOnlyRoots roots(heap_);
  if (strong_gc_subroot_names_.is_empty()) {
#define NAME_ENTRY(name) strong_gc_subroot_names_.SetTag(heap_->name(), #name);
#define RO_NAME_ENTRY(name) \
  strong_gc_subroot_names_.SetTag(roots.name(), #name);
#define ROOT_NAME(type, name, camel_name) NAME_ENTRY(name)
    STRONG_MUTABLE_ROOT_LIST(ROOT_NAME)
#undef ROOT_NAME
#define ROOT_NAME(type, name, camel_name) RO_NAME_ENTRY(name)
    STRONG_READ_ONLY_ROOT_LIST(ROOT_NAME)
#undef ROOT_NAME
#define STRUCT_MAP_NAME(NAME, Name, name) RO_NAME_ENTRY(name##_map)
    STRUCT_LIST(STRUCT_MAP_NAME)
#undef STRUCT_MAP_NAME
#define ALLOCATION_SITE_MAP_NAME(NAME, Name, Size, name) \
  RO_NAME_ENTRY(name##_map)
    ALLOCATION_SITE_LIST(ALLOCATION_SITE_MAP_NAME)
#undef ALLOCATION_SITE_MAP_NAME
#define DATA_HANDLER_MAP_NAME(NAME, Name, Size, name) NAME_ENTRY(name##_map)
    DATA_HANDLER_LIST(DATA_HANDLER_MAP_NAME)
#undef DATA_HANDLER_MAP_NAME
#define STRING_NAME(name, str) RO_NAME_ENTRY(name)
    INTERNALIZED_STRING_LIST(STRING_NAME)
#undef STRING_NAME
#define SYMBOL_NAME(name) RO_NAME_ENTRY(name)
    PRIVATE_SYMBOL_LIST(SYMBOL_NAME)
#undef SYMBOL_NAME
#define SYMBOL_NAME(name, description) RO_NAME_ENTRY(name)
    PUBLIC_SYMBOL_LIST(SYMBOL_NAME)
    WELL_KNOWN_SYMBOL_LIST(SYMBOL_NAME)
#undef SYMBOL_NAME
#define ACCESSOR_NAME(accessor_name, AccessorName) \
  NAME_ENTRY(accessor_name##_accessor)
    ACCESSOR_INFO_LIST(ACCESSOR_NAME)
#undef ACCESSOR_NAME
#undef NAME_ENTRY
#undef RO_NAME_ENTRY
    CHECK(!strong_gc_subroot_names_.is_empty());
  }
  return strong_gc_subroot_names_.GetTag(object);
}

void V8HeapExplorer::TagObject(Object* obj, const char* tag) {
  if (IsEssentialObject(obj)) {
    HeapEntry* entry = GetEntry(obj);
    if (entry->name()[0] == '\0') {
      entry->set_name(tag);
    }
  }
}

class GlobalObjectsEnumerator : public RootVisitor {
 public:
  void VisitRootPointers(Root root, const char* description, Object** start,
                         Object** end) override {
    for (Object** p = start; p < end; p++) {
      if (!(*p)->IsNativeContext()) continue;
      JSObject* proxy = Context::cast(*p)->global_proxy();
      if (!proxy->IsJSGlobalProxy()) continue;
      Object* global = proxy->map()->prototype();
      if (!global->IsJSGlobalObject()) continue;
      objects_.push_back(Handle<JSGlobalObject>(JSGlobalObject::cast(global),
                                                proxy->GetIsolate()));
    }
  }
  int count() const { return static_cast<int>(objects_.size()); }
  Handle<JSGlobalObject>& at(int i) { return objects_[i]; }

 private:
  std::vector<Handle<JSGlobalObject>> objects_;
};


// Modifies heap. Must not be run during heap traversal.
void V8HeapExplorer::TagGlobalObjects() {
  Isolate* isolate = heap_->isolate();
  HandleScope scope(isolate);
  GlobalObjectsEnumerator enumerator;
  isolate->global_handles()->IterateAllRoots(&enumerator);
  std::vector<const char*> urls(enumerator.count());
  for (int i = 0, l = enumerator.count(); i < l; ++i) {
    urls[i] = global_object_name_resolver_
                  ? global_object_name_resolver_->GetName(Utils::ToLocal(
                        Handle<JSObject>::cast(enumerator.at(i))))
                  : nullptr;
  }

  DisallowHeapAllocation no_allocation;
  for (int i = 0, l = enumerator.count(); i < l; ++i) {
    objects_tags_.SetTag(*enumerator.at(i), urls[i]);
  }
}

class EmbedderGraphImpl : public EmbedderGraph {
 public:
  struct Edge {
    Node* from;
    Node* to;
    const char* name;
  };

  class V8NodeImpl : public Node {
   public:
    explicit V8NodeImpl(Object* object) : object_(object) {}
    Object* GetObject() { return object_; }

    // Node overrides.
    bool IsEmbedderNode() override { return false; }
    const char* Name() override {
      // The name should be retrieved via GetObject().
      UNREACHABLE();
      return "";
    }
    size_t SizeInBytes() override {
      // The size should be retrieved via GetObject().
      UNREACHABLE();
      return 0;
    }

   private:
    Object* object_;
  };

  Node* V8Node(const v8::Local<v8::Value>& value) final {
    Handle<Object> object = v8::Utils::OpenHandle(*value);
    DCHECK(!object.is_null());
    return AddNode(std::unique_ptr<Node>(new V8NodeImpl(*object)));
  }

  Node* AddNode(std::unique_ptr<Node> node) final {
    Node* result = node.get();
    nodes_.push_back(std::move(node));
    return result;
  }

  void AddEdge(Node* from, Node* to, const char* name) final {
    edges_.push_back({from, to, name});
  }

  const std::vector<std::unique_ptr<Node>>& nodes() { return nodes_; }
  const std::vector<Edge>& edges() { return edges_; }

 private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Edge> edges_;
};

class GlobalHandlesExtractor : public PersistentHandleVisitor {
 public:
  explicit GlobalHandlesExtractor(NativeObjectsExplorer* explorer)
      : explorer_(explorer) {}
  ~GlobalHandlesExtractor() override {}
  void VisitPersistentHandle(Persistent<Value>* value,
                             uint16_t class_id) override {
    Handle<Object> object = Utils::OpenPersistent(value);
    explorer_->VisitSubtreeWrapper(object.location(), class_id);
  }

 private:
  NativeObjectsExplorer* explorer_;
};


class BasicHeapEntriesAllocator : public HeapEntriesAllocator {
 public:
  BasicHeapEntriesAllocator(
      HeapSnapshot* snapshot,
      HeapEntry::Type entries_type)
    : snapshot_(snapshot),
      names_(snapshot_->profiler()->names()),
      heap_object_map_(snapshot_->profiler()->heap_object_map()),
      entries_type_(entries_type) {
  }
  virtual HeapEntry* AllocateEntry(HeapThing ptr);
 private:
  HeapSnapshot* snapshot_;
  StringsStorage* names_;
  HeapObjectsMap* heap_object_map_;
  HeapEntry::Type entries_type_;
};


HeapEntry* BasicHeapEntriesAllocator::AllocateEntry(HeapThing ptr) {
  v8::RetainedObjectInfo* info = reinterpret_cast<v8::RetainedObjectInfo*>(ptr);
  intptr_t elements = info->GetElementCount();
  intptr_t size = info->GetSizeInBytes();
  const char* name = elements != -1
                         ? names_->GetFormatted("%s / %" V8PRIdPTR " entries",
                                                info->GetLabel(), elements)
                         : names_->GetCopy(info->GetLabel());
  return snapshot_->AddEntry(
      entries_type_,
      name,
      heap_object_map_->GenerateId(info),
      size != -1 ? static_cast<int>(size) : 0,
      0);
}

class EmbedderGraphEntriesAllocator : public HeapEntriesAllocator {
 public:
  explicit EmbedderGraphEntriesAllocator(HeapSnapshot* snapshot)
      : snapshot_(snapshot),
        names_(snapshot_->profiler()->names()),
        heap_object_map_(snapshot_->profiler()->heap_object_map()) {}
  virtual HeapEntry* AllocateEntry(HeapThing ptr);

 private:
  HeapSnapshot* snapshot_;
  StringsStorage* names_;
  HeapObjectsMap* heap_object_map_;
};

namespace {

const char* EmbedderGraphNodeName(StringsStorage* names,
                                  EmbedderGraphImpl::Node* node) {
  const char* prefix = node->NamePrefix();
  return prefix ? names->GetFormatted("%s %s", prefix, node->Name())
                : names->GetCopy(node->Name());
}

HeapEntry::Type EmbedderGraphNodeType(EmbedderGraphImpl::Node* node) {
  return HeapEntry::kNative;
}

// Merges the names of an embedder node and its wrapper node.
// If the wrapper node name contains a tag suffix (part after '/') then the
// result is the embedder node name concatenated with the tag suffix.
// Otherwise, the result is the embedder node name.
const char* MergeNames(StringsStorage* names, const char* embedder_name,
                       const char* wrapper_name) {
  for (const char* suffix = wrapper_name; *suffix; suffix++) {
    if (*suffix == '/') {
      return names->GetFormatted("%s %s", embedder_name, suffix);
    }
  }
  return embedder_name;
}

}  // anonymous namespace

HeapEntry* EmbedderGraphEntriesAllocator::AllocateEntry(HeapThing ptr) {
  EmbedderGraphImpl::Node* node =
      reinterpret_cast<EmbedderGraphImpl::Node*>(ptr);
  DCHECK(node->IsEmbedderNode());
  size_t size = node->SizeInBytes();
  return snapshot_->AddEntry(
      EmbedderGraphNodeType(node), EmbedderGraphNodeName(names_, node),
      static_cast<SnapshotObjectId>(reinterpret_cast<uintptr_t>(node) << 1),
      static_cast<int>(size), 0);
}

class NativeGroupRetainedObjectInfo : public v8::RetainedObjectInfo {
 public:
  explicit NativeGroupRetainedObjectInfo(const char* label)
      : disposed_(false),
        hash_(reinterpret_cast<intptr_t>(label)),
        label_(label) {}

  virtual ~NativeGroupRetainedObjectInfo() {}
  virtual void Dispose() {
    CHECK(!disposed_);
    disposed_ = true;
    delete this;
  }
  virtual bool IsEquivalent(RetainedObjectInfo* other) {
    return hash_ == other->GetHash() && !strcmp(label_, other->GetLabel());
  }
  virtual intptr_t GetHash() { return hash_; }
  virtual const char* GetLabel() { return label_; }

 private:
  bool disposed_;
  intptr_t hash_;
  const char* label_;
};

NativeObjectsExplorer::NativeObjectsExplorer(
    HeapSnapshot* snapshot, SnapshottingProgressReportingInterface* progress)
    : isolate_(snapshot->profiler()->heap_object_map()->heap()->isolate()),
      snapshot_(snapshot),
      names_(snapshot_->profiler()->names()),
      embedder_queried_(false),
      native_groups_(0, SeededStringHasher(isolate_->heap()->HashSeed())),
      synthetic_entries_allocator_(
          new BasicHeapEntriesAllocator(snapshot, HeapEntry::kSynthetic)),
      native_entries_allocator_(
          new BasicHeapEntriesAllocator(snapshot, HeapEntry::kNative)),
      embedder_graph_entries_allocator_(
          new EmbedderGraphEntriesAllocator(snapshot)),
      filler_(nullptr) {}

NativeObjectsExplorer::~NativeObjectsExplorer() {
  for (auto map_entry : objects_by_info_) {
    v8::RetainedObjectInfo* info = map_entry.first;
    info->Dispose();
    std::vector<HeapObject*>* objects = map_entry.second;
    delete objects;
  }
  for (auto map_entry : native_groups_) {
    NativeGroupRetainedObjectInfo* info = map_entry.second;
    info->Dispose();
  }
}


int NativeObjectsExplorer::EstimateObjectsCount() {
  FillRetainedObjects();
  return static_cast<int>(objects_by_info_.size());
}


void NativeObjectsExplorer::FillRetainedObjects() {
  if (embedder_queried_) return;
  v8::HandleScope scope(reinterpret_cast<v8::Isolate*>(isolate_));
  v8::HeapProfiler::RetainerInfos infos =
      snapshot_->profiler()->GetRetainerInfos(isolate_);
  for (auto& pair : infos.groups) {
    std::vector<HeapObject*>* info = GetVectorMaybeDisposeInfo(pair.first);
    for (auto& persistent : pair.second) {
      if (persistent->IsEmpty()) continue;

      Handle<Object> object = v8::Utils::OpenHandle(
          *persistent->Get(reinterpret_cast<v8::Isolate*>(isolate_)));
      DCHECK(!object.is_null());
      HeapObject* heap_object = HeapObject::cast(*object);
      info->push_back(heap_object);
      in_groups_.Insert(heap_object);
    }
  }

  // Record objects that are not in ObjectGroups, but have class ID.
  GlobalHandlesExtractor extractor(this);
  isolate_->global_handles()->IterateAllRootsWithClassIds(&extractor);

  edges_ = std::move(infos.edges);
  embedder_queried_ = true;
}

void NativeObjectsExplorer::FillEdges() {
  v8::HandleScope scope(reinterpret_cast<v8::Isolate*>(isolate_));
  // Fill in actual edges found.
  for (auto& pair : edges_) {
    if (pair.first->IsEmpty() || pair.second->IsEmpty()) continue;

    Handle<Object> parent_object = v8::Utils::OpenHandle(
        *pair.first->Get(reinterpret_cast<v8::Isolate*>(isolate_)));
    HeapObject* parent = HeapObject::cast(*parent_object);
    int parent_entry =
        filler_->FindOrAddEntry(parent, native_entries_allocator_.get())
            ->index();
    DCHECK_NE(parent_entry, HeapEntry::kNoEntry);
    Handle<Object> child_object = v8::Utils::OpenHandle(
        *pair.second->Get(reinterpret_cast<v8::Isolate*>(isolate_)));
    HeapObject* child = HeapObject::cast(*child_object);
    HeapEntry* child_entry =
        filler_->FindOrAddEntry(child, native_entries_allocator_.get());
    filler_->SetNamedReference(HeapGraphEdge::kInternal, parent_entry, "native",
                               child_entry);
  }
  edges_.clear();
}

std::vector<HeapObject*>* NativeObjectsExplorer::GetVectorMaybeDisposeInfo(
    v8::RetainedObjectInfo* info) {
  auto map_entry = objects_by_info_.find(info);
  if (map_entry != objects_by_info_.end()) {
    info->Dispose();
  } else {
    objects_by_info_[info] = new std::vector<HeapObject*>();
  }
  return objects_by_info_[info];
}

HeapEntry* NativeObjectsExplorer::EntryForEmbedderGraphNode(
    EmbedderGraphImpl::Node* node) {
  EmbedderGraphImpl::Node* wrapper = node->WrapperNode();
  if (wrapper) {
    node = wrapper;
  }
  if (node->IsEmbedderNode()) {
    return filler_->FindOrAddEntry(node,
                                   embedder_graph_entries_allocator_.get());
  } else {
    EmbedderGraphImpl::V8NodeImpl* v8_node =
        static_cast<EmbedderGraphImpl::V8NodeImpl*>(node);
    Object* object = v8_node->GetObject();
    if (object->IsSmi()) return nullptr;
    HeapEntry* entry = filler_->FindEntry(HeapObject::cast(object));
    return entry;
  }
}

bool NativeObjectsExplorer::IterateAndExtractReferences(
    SnapshotFiller* filler) {
  filler_ = filler;

  if (FLAG_heap_profiler_use_embedder_graph &&
      snapshot_->profiler()->HasBuildEmbedderGraphCallback()) {
    v8::HandleScope scope(reinterpret_cast<v8::Isolate*>(isolate_));
    DisallowHeapAllocation no_allocation;
    EmbedderGraphImpl graph;
    snapshot_->profiler()->BuildEmbedderGraph(isolate_, &graph);
    for (const auto& node : graph.nodes()) {
      if (node->IsRootNode()) {
        filler_->SetIndexedAutoIndexReference(
            HeapGraphEdge::kElement, snapshot_->root()->index(),
            EntryForEmbedderGraphNode(node.get()));
      }
      // Adjust the name and the type of the V8 wrapper node.
      auto wrapper = node->WrapperNode();
      if (wrapper) {
        HeapEntry* wrapper_entry = EntryForEmbedderGraphNode(wrapper);
        wrapper_entry->set_name(
            MergeNames(names_, EmbedderGraphNodeName(names_, node.get()),
                       wrapper_entry->name()));
        wrapper_entry->set_type(EmbedderGraphNodeType(node.get()));
      }
    }
    // Fill edges of the graph.
    for (const auto& edge : graph.edges()) {
      HeapEntry* from = EntryForEmbedderGraphNode(edge.from);
      // The |from| and |to| can nullptr if the corrsponding node is a V8 node
      // pointing to a Smi.
      if (!from) continue;
      // Adding an entry for |edge.to| can invalidate the |from| entry because
      // it is an address in std::vector. Use index instead of pointer.
      int from_index = from->index();
      HeapEntry* to = EntryForEmbedderGraphNode(edge.to);
      if (to) {
        if (edge.name == nullptr) {
          filler_->SetIndexedAutoIndexReference(HeapGraphEdge::kElement,
                                                from_index, to);
        } else {
          filler_->SetNamedReference(HeapGraphEdge::kInternal, from_index,
                                     edge.name, to);
        }
      }
    }
  } else {
    FillRetainedObjects();
    FillEdges();
    if (EstimateObjectsCount() > 0) {
      for (auto map_entry : objects_by_info_) {
        v8::RetainedObjectInfo* info = map_entry.first;
        SetNativeRootReference(info);
        std::vector<HeapObject*>* objects = map_entry.second;
        for (HeapObject* object : *objects) {
          SetWrapperNativeReferences(object, info);
        }
      }
      SetRootNativeRootsReference();
    }
  }
  filler_ = nullptr;
  return true;
}

NativeGroupRetainedObjectInfo* NativeObjectsExplorer::FindOrAddGroupInfo(
    const char* label) {
  const char* label_copy = names_->GetCopy(label);
  auto map_entry = native_groups_.find(label_copy);
  if (map_entry == native_groups_.end()) {
    native_groups_[label_copy] = new NativeGroupRetainedObjectInfo(label);
  }
  return native_groups_[label_copy];
}

void NativeObjectsExplorer::SetNativeRootReference(
    v8::RetainedObjectInfo* info) {
  HeapEntry* child_entry =
      filler_->FindOrAddEntry(info, native_entries_allocator_.get());
  DCHECK_NOT_NULL(child_entry);
  NativeGroupRetainedObjectInfo* group_info =
      FindOrAddGroupInfo(info->GetGroupLabel());
  HeapEntry* group_entry =
      filler_->FindOrAddEntry(group_info, synthetic_entries_allocator_.get());
  // |FindOrAddEntry| can move and resize the entries backing store. Reload
  // potentially-stale pointer.
  child_entry = filler_->FindEntry(info);
  filler_->SetNamedAutoIndexReference(
      HeapGraphEdge::kInternal, group_entry->index(), nullptr, child_entry);
}


void NativeObjectsExplorer::SetWrapperNativeReferences(
    HeapObject* wrapper, v8::RetainedObjectInfo* info) {
  HeapEntry* wrapper_entry = filler_->FindEntry(wrapper);
  DCHECK_NOT_NULL(wrapper_entry);
  HeapEntry* info_entry =
      filler_->FindOrAddEntry(info, native_entries_allocator_.get());
  DCHECK_NOT_NULL(info_entry);
  filler_->SetNamedReference(HeapGraphEdge::kInternal,
                             wrapper_entry->index(),
                             "native",
                             info_entry);
  filler_->SetIndexedAutoIndexReference(HeapGraphEdge::kElement,
                                        info_entry->index(),
                                        wrapper_entry);
}


void NativeObjectsExplorer::SetRootNativeRootsReference() {
  for (auto map_entry : native_groups_) {
    NativeGroupRetainedObjectInfo* group_info = map_entry.second;
    HeapEntry* group_entry =
        filler_->FindOrAddEntry(group_info, native_entries_allocator_.get());
    DCHECK_NOT_NULL(group_entry);
    filler_->SetIndexedAutoIndexReference(
        HeapGraphEdge::kElement,
        snapshot_->root()->index(),
        group_entry);
  }
}


void NativeObjectsExplorer::VisitSubtreeWrapper(Object** p, uint16_t class_id) {
  if (in_groups_.Contains(*p)) return;
  Isolate* isolate = isolate_;
  v8::RetainedObjectInfo* info =
      isolate->heap_profiler()->ExecuteWrapperClassCallback(class_id, p);
  if (info == nullptr) return;
  GetVectorMaybeDisposeInfo(info)->push_back(HeapObject::cast(*p));
}


HeapSnapshotGenerator::HeapSnapshotGenerator(
    HeapSnapshot* snapshot,
    v8::ActivityControl* control,
    v8::HeapProfiler::ObjectNameResolver* resolver,
    Heap* heap)
    : snapshot_(snapshot),
      control_(control),
      v8_heap_explorer_(snapshot_, this, resolver),
      dom_explorer_(snapshot_, this),
      heap_(heap) {
}

namespace {
class NullContextScope {
 public:
  explicit NullContextScope(Isolate* isolate)
      : isolate_(isolate), prev_(isolate->context()) {
    isolate_->set_context(nullptr);
  }
  ~NullContextScope() { isolate_->set_context(prev_); }

 private:
  Isolate* isolate_;
  Context* prev_;
};
}  //  namespace

bool HeapSnapshotGenerator::GenerateSnapshot() {
  v8_heap_explorer_.TagGlobalObjects();

  // TODO(1562) Profiler assumes that any object that is in the heap after
  // full GC is reachable from the root when computing dominators.
  // This is not true for weakly reachable objects.
  // As a temporary solution we call GC twice.
  heap_->CollectAllGarbage(Heap::kMakeHeapIterableMask,
                           GarbageCollectionReason::kHeapProfiler);
  heap_->CollectAllGarbage(Heap::kMakeHeapIterableMask,
                           GarbageCollectionReason::kHeapProfiler);

  NullContextScope null_context_scope(heap_->isolate());

#ifdef VERIFY_HEAP
  Heap* debug_heap = heap_;
  if (FLAG_verify_heap) {
    debug_heap->Verify();
  }
#endif

  InitProgressCounter();

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    debug_heap->Verify();
  }
#endif

  snapshot_->AddSyntheticRootEntries();

  if (!FillReferences()) return false;

  snapshot_->FillChildren();
  snapshot_->RememberLastJSObjectId();

  progress_counter_ = progress_total_;
  if (!ProgressReport(true)) return false;
  return true;
}

void HeapSnapshotGenerator::ProgressStep() {
  ++progress_counter_;
}

bool HeapSnapshotGenerator::ProgressReport(bool force) {
  const int kProgressReportGranularity = 10000;
  if (control_ != nullptr &&
      (force || progress_counter_ % kProgressReportGranularity == 0)) {
    return control_->ReportProgressValue(progress_counter_, progress_total_) ==
           v8::ActivityControl::kContinue;
  }
  return true;
}

void HeapSnapshotGenerator::InitProgressCounter() {
  if (control_ == nullptr) return;
  // The +1 ensures that intermediate ProgressReport calls will never signal
  // that the work is finished (i.e. progress_counter_ == progress_total_).
  // Only the forced ProgressReport() at the end of GenerateSnapshot()
  // should signal that the work is finished because signalling finished twice
  // breaks the DevTools frontend.
  progress_total_ = v8_heap_explorer_.EstimateObjectsCount() +
                    dom_explorer_.EstimateObjectsCount() + 1;
  progress_counter_ = 0;
}

bool HeapSnapshotGenerator::FillReferences() {
  SnapshotFiller filler(snapshot_, &entries_);
  return v8_heap_explorer_.IterateAndExtractReferences(&filler) &&
         dom_explorer_.IterateAndExtractReferences(&filler);
}


template<int bytes> struct MaxDecimalDigitsIn;
template<> struct MaxDecimalDigitsIn<4> {
  static const int kSigned = 11;
  static const int kUnsigned = 10;
};
template<> struct MaxDecimalDigitsIn<8> {
  static const int kSigned = 20;
  static const int kUnsigned = 20;
};


class OutputStreamWriter {
 public:
  explicit OutputStreamWriter(v8::OutputStream* stream)
      : stream_(stream),
        chunk_size_(stream->GetChunkSize()),
        chunk_(chunk_size_),
        chunk_pos_(0),
        aborted_(false) {
    DCHECK_GT(chunk_size_, 0);
  }
  bool aborted() { return aborted_; }
  void AddCharacter(char c) {
    DCHECK_NE(c, '\0');
    DCHECK(chunk_pos_ < chunk_size_);
    chunk_[chunk_pos_++] = c;
    MaybeWriteChunk();
  }
  void AddString(const char* s) {
    AddSubstring(s, StrLength(s));
  }
  void AddSubstring(const char* s, int n) {
    if (n <= 0) return;
    DCHECK(static_cast<size_t>(n) <= strlen(s));
    const char* s_end = s + n;
    while (s < s_end) {
      int s_chunk_size =
          Min(chunk_size_ - chunk_pos_, static_cast<int>(s_end - s));
      DCHECK_GT(s_chunk_size, 0);
      MemCopy(chunk_.start() + chunk_pos_, s, s_chunk_size);
      s += s_chunk_size;
      chunk_pos_ += s_chunk_size;
      MaybeWriteChunk();
    }
  }
  void AddNumber(unsigned n) { AddNumberImpl<unsigned>(n, "%u"); }
  void Finalize() {
    if (aborted_) return;
    DCHECK(chunk_pos_ < chunk_size_);
    if (chunk_pos_ != 0) {
      WriteChunk();
    }
    stream_->EndOfStream();
  }

 private:
  template<typename T>
  void AddNumberImpl(T n, const char* format) {
    // Buffer for the longest value plus trailing \0
    static const int kMaxNumberSize =
        MaxDecimalDigitsIn<sizeof(T)>::kUnsigned + 1;
    if (chunk_size_ - chunk_pos_ >= kMaxNumberSize) {
      int result = SNPrintF(
          chunk_.SubVector(chunk_pos_, chunk_size_), format, n);
      DCHECK_NE(result, -1);
      chunk_pos_ += result;
      MaybeWriteChunk();
    } else {
      EmbeddedVector<char, kMaxNumberSize> buffer;
      int result = SNPrintF(buffer, format, n);
      USE(result);
      DCHECK_NE(result, -1);
      AddString(buffer.start());
    }
  }
  void MaybeWriteChunk() {
    DCHECK(chunk_pos_ <= chunk_size_);
    if (chunk_pos_ == chunk_size_) {
      WriteChunk();
    }
  }
  void WriteChunk() {
    if (aborted_) return;
    if (stream_->WriteAsciiChunk(chunk_.start(), chunk_pos_) ==
        v8::OutputStream::kAbort) aborted_ = true;
    chunk_pos_ = 0;
  }

  v8::OutputStream* stream_;
  int chunk_size_;
  ScopedVector<char> chunk_;
  int chunk_pos_;
  bool aborted_;
};


// type, name|index, to_node.
const int HeapSnapshotJSONSerializer::kEdgeFieldsCount = 3;
// type, name, id, self_size, edge_count, trace_node_id.
const int HeapSnapshotJSONSerializer::kNodeFieldsCount = 6;

void HeapSnapshotJSONSerializer::Serialize(v8::OutputStream* stream) {
  if (AllocationTracker* allocation_tracker =
      snapshot_->profiler()->allocation_tracker()) {
    allocation_tracker->PrepareForSerialization();
  }
  DCHECK_NULL(writer_);
  writer_ = new OutputStreamWriter(stream);
  SerializeImpl();
  delete writer_;
  writer_ = nullptr;
}


void HeapSnapshotJSONSerializer::SerializeImpl() {
  DCHECK_EQ(0, snapshot_->root()->index());
  writer_->AddCharacter('{');
  writer_->AddString("\"snapshot\":{");
  SerializeSnapshot();
  if (writer_->aborted()) return;
  writer_->AddString("},\n");
  writer_->AddString("\"nodes\":[");
  SerializeNodes();
  if (writer_->aborted()) return;
  writer_->AddString("],\n");
  writer_->AddString("\"edges\":[");
  SerializeEdges();
  if (writer_->aborted()) return;
  writer_->AddString("],\n");

  writer_->AddString("\"trace_function_infos\":[");
  SerializeTraceNodeInfos();
  if (writer_->aborted()) return;
  writer_->AddString("],\n");
  writer_->AddString("\"trace_tree\":[");
  SerializeTraceTree();
  if (writer_->aborted()) return;
  writer_->AddString("],\n");

  writer_->AddString("\"samples\":[");
  SerializeSamples();
  if (writer_->aborted()) return;
  writer_->AddString("],\n");

  writer_->AddString("\"strings\":[");
  SerializeStrings();
  if (writer_->aborted()) return;
  writer_->AddCharacter(']');
  writer_->AddCharacter('}');
  writer_->Finalize();
}


int HeapSnapshotJSONSerializer::GetStringId(const char* s) {
  base::HashMap::Entry* cache_entry =
      strings_.LookupOrInsert(const_cast<char*>(s), StringHash(s));
  if (cache_entry->value == nullptr) {
    cache_entry->value = reinterpret_cast<void*>(next_string_id_++);
  }
  return static_cast<int>(reinterpret_cast<intptr_t>(cache_entry->value));
}


namespace {

template<size_t size> struct ToUnsigned;

template<> struct ToUnsigned<4> {
  typedef uint32_t Type;
};

template<> struct ToUnsigned<8> {
  typedef uint64_t Type;
};

}  // namespace


template<typename T>
static int utoa_impl(T value, const Vector<char>& buffer, int buffer_pos) {
  STATIC_ASSERT(static_cast<T>(-1) > 0);  // Check that T is unsigned
  int number_of_digits = 0;
  T t = value;
  do {
    ++number_of_digits;
  } while (t /= 10);

  buffer_pos += number_of_digits;
  int result = buffer_pos;
  do {
    int last_digit = static_cast<int>(value % 10);
    buffer[--buffer_pos] = '0' + last_digit;
    value /= 10;
  } while (value);
  return result;
}


template<typename T>
static int utoa(T value, const Vector<char>& buffer, int buffer_pos) {
  typename ToUnsigned<sizeof(value)>::Type unsigned_value = value;
  STATIC_ASSERT(sizeof(value) == sizeof(unsigned_value));
  return utoa_impl(unsigned_value, buffer, buffer_pos);
}


void HeapSnapshotJSONSerializer::SerializeEdge(HeapGraphEdge* edge,
                                               bool first_edge) {
  // The buffer needs space for 3 unsigned ints, 3 commas, \n and \0
  static const int kBufferSize =
      MaxDecimalDigitsIn<sizeof(unsigned)>::kUnsigned * 3 + 3 + 2;  // NOLINT
  EmbeddedVector<char, kBufferSize> buffer;
  int edge_name_or_index = edge->type() == HeapGraphEdge::kElement
      || edge->type() == HeapGraphEdge::kHidden
      ? edge->index() : GetStringId(edge->name());
  int buffer_pos = 0;
  if (!first_edge) {
    buffer[buffer_pos++] = ',';
  }
  buffer_pos = utoa(edge->type(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(edge_name_or_index, buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(entry_index(edge->to()), buffer, buffer_pos);
  buffer[buffer_pos++] = '\n';
  buffer[buffer_pos++] = '\0';
  writer_->AddString(buffer.start());
}


void HeapSnapshotJSONSerializer::SerializeEdges() {
  std::deque<HeapGraphEdge*>& edges = snapshot_->children();
  for (size_t i = 0; i < edges.size(); ++i) {
    DCHECK(i == 0 ||
           edges[i - 1]->from()->index() <= edges[i]->from()->index());
    SerializeEdge(edges[i], i == 0);
    if (writer_->aborted()) return;
  }
}

void HeapSnapshotJSONSerializer::SerializeNode(const HeapEntry* entry) {
  // The buffer needs space for 4 unsigned ints, 1 size_t, 5 commas, \n and \0
  static const int kBufferSize =
      5 * MaxDecimalDigitsIn<sizeof(unsigned)>::kUnsigned  // NOLINT
      + MaxDecimalDigitsIn<sizeof(size_t)>::kUnsigned  // NOLINT
      + 6 + 1 + 1;
  EmbeddedVector<char, kBufferSize> buffer;
  int buffer_pos = 0;
  if (entry_index(entry) != 0) {
    buffer[buffer_pos++] = ',';
  }
  buffer_pos = utoa(entry->type(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(GetStringId(entry->name()), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(entry->id(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(entry->self_size(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(entry->children_count(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(entry->trace_node_id(), buffer, buffer_pos);
  buffer[buffer_pos++] = '\n';
  buffer[buffer_pos++] = '\0';
  writer_->AddString(buffer.start());
}


void HeapSnapshotJSONSerializer::SerializeNodes() {
  std::vector<HeapEntry>& entries = snapshot_->entries();
  for (const HeapEntry& entry : entries) {
    SerializeNode(&entry);
    if (writer_->aborted()) return;
  }
}


void HeapSnapshotJSONSerializer::SerializeSnapshot() {
  writer_->AddString("\"meta\":");
  // The object describing node serialization layout.
  // We use a set of macros to improve readability.
#define JSON_A(s) "[" s "]"
#define JSON_O(s) "{" s "}"
#define JSON_S(s) "\"" s "\""
  writer_->AddString(JSON_O(
    JSON_S("node_fields") ":" JSON_A(
        JSON_S("type") ","
        JSON_S("name") ","
        JSON_S("id") ","
        JSON_S("self_size") ","
        JSON_S("edge_count") ","
        JSON_S("trace_node_id")) ","
    JSON_S("node_types") ":" JSON_A(
        JSON_A(
            JSON_S("hidden") ","
            JSON_S("array") ","
            JSON_S("string") ","
            JSON_S("object") ","
            JSON_S("code") ","
            JSON_S("closure") ","
            JSON_S("regexp") ","
            JSON_S("number") ","
            JSON_S("native") ","
            JSON_S("synthetic") ","
            JSON_S("concatenated string") ","
            JSON_S("sliced string") ","
            JSON_S("symbol") ","
            JSON_S("bigint")) ","
        JSON_S("string") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number") ","
        JSON_S("number")) ","
    JSON_S("edge_fields") ":" JSON_A(
        JSON_S("type") ","
        JSON_S("name_or_index") ","
        JSON_S("to_node")) ","
    JSON_S("edge_types") ":" JSON_A(
        JSON_A(
            JSON_S("context") ","
            JSON_S("element") ","
            JSON_S("property") ","
            JSON_S("internal") ","
            JSON_S("hidden") ","
            JSON_S("shortcut") ","
            JSON_S("weak")) ","
        JSON_S("string_or_number") ","
        JSON_S("node")) ","
    JSON_S("trace_function_info_fields") ":" JSON_A(
        JSON_S("function_id") ","
        JSON_S("name") ","
        JSON_S("script_name") ","
        JSON_S("script_id") ","
        JSON_S("line") ","
        JSON_S("column")) ","
    JSON_S("trace_node_fields") ":" JSON_A(
        JSON_S("id") ","
        JSON_S("function_info_index") ","
        JSON_S("count") ","
        JSON_S("size") ","
        JSON_S("children")) ","
    JSON_S("sample_fields") ":" JSON_A(
        JSON_S("timestamp_us") ","
        JSON_S("last_assigned_id"))));
#undef JSON_S
#undef JSON_O
#undef JSON_A
  writer_->AddString(",\"node_count\":");
  writer_->AddNumber(static_cast<unsigned>(snapshot_->entries().size()));
  writer_->AddString(",\"edge_count\":");
  writer_->AddNumber(static_cast<double>(snapshot_->edges().size()));
  writer_->AddString(",\"trace_function_count\":");
  uint32_t count = 0;
  AllocationTracker* tracker = snapshot_->profiler()->allocation_tracker();
  if (tracker) {
    count = static_cast<uint32_t>(tracker->function_info_list().size());
  }
  writer_->AddNumber(count);
}


static void WriteUChar(OutputStreamWriter* w, unibrow::uchar u) {
  static const char hex_chars[] = "0123456789ABCDEF";
  w->AddString("\\u");
  w->AddCharacter(hex_chars[(u >> 12) & 0xF]);
  w->AddCharacter(hex_chars[(u >> 8) & 0xF]);
  w->AddCharacter(hex_chars[(u >> 4) & 0xF]);
  w->AddCharacter(hex_chars[u & 0xF]);
}


void HeapSnapshotJSONSerializer::SerializeTraceTree() {
  AllocationTracker* tracker = snapshot_->profiler()->allocation_tracker();
  if (!tracker) return;
  AllocationTraceTree* traces = tracker->trace_tree();
  SerializeTraceNode(traces->root());
}


void HeapSnapshotJSONSerializer::SerializeTraceNode(AllocationTraceNode* node) {
  // The buffer needs space for 4 unsigned ints, 4 commas, [ and \0
  const int kBufferSize =
      4 * MaxDecimalDigitsIn<sizeof(unsigned)>::kUnsigned  // NOLINT
      + 4 + 1 + 1;
  EmbeddedVector<char, kBufferSize> buffer;
  int buffer_pos = 0;
  buffer_pos = utoa(node->id(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(node->function_info_index(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(node->allocation_count(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer_pos = utoa(node->allocation_size(), buffer, buffer_pos);
  buffer[buffer_pos++] = ',';
  buffer[buffer_pos++] = '[';
  buffer[buffer_pos++] = '\0';
  writer_->AddString(buffer.start());

  int i = 0;
  for (AllocationTraceNode* child : node->children()) {
    if (i++ > 0) {
      writer_->AddCharacter(',');
    }
    SerializeTraceNode(child);
  }
  writer_->AddCharacter(']');
}


// 0-based position is converted to 1-based during the serialization.
static int SerializePosition(int position, const Vector<char>& buffer,
                             int buffer_pos) {
  if (position == -1) {
    buffer[buffer_pos++] = '0';
  } else {
    DCHECK_GE(position, 0);
    buffer_pos = utoa(static_cast<unsigned>(position + 1), buffer, buffer_pos);
  }
  return buffer_pos;
}


void HeapSnapshotJSONSerializer::SerializeTraceNodeInfos() {
  AllocationTracker* tracker = snapshot_->profiler()->allocation_tracker();
  if (!tracker) return;
  // The buffer needs space for 6 unsigned ints, 6 commas, \n and \0
  const int kBufferSize =
      6 * MaxDecimalDigitsIn<sizeof(unsigned)>::kUnsigned  // NOLINT
      + 6 + 1 + 1;
  EmbeddedVector<char, kBufferSize> buffer;
  int i = 0;
  for (AllocationTracker::FunctionInfo* info : tracker->function_info_list()) {
    int buffer_pos = 0;
    if (i++ > 0) {
      buffer[buffer_pos++] = ',';
    }
    buffer_pos = utoa(info->function_id, buffer, buffer_pos);
    buffer[buffer_pos++] = ',';
    buffer_pos = utoa(GetStringId(info->name), buffer, buffer_pos);
    buffer[buffer_pos++] = ',';
    buffer_pos = utoa(GetStringId(info->script_name), buffer, buffer_pos);
    buffer[buffer_pos++] = ',';
    // The cast is safe because script id is a non-negative Smi.
    buffer_pos = utoa(static_cast<unsigned>(info->script_id), buffer,
        buffer_pos);
    buffer[buffer_pos++] = ',';
    buffer_pos = SerializePosition(info->line, buffer, buffer_pos);
    buffer[buffer_pos++] = ',';
    buffer_pos = SerializePosition(info->column, buffer, buffer_pos);
    buffer[buffer_pos++] = '\n';
    buffer[buffer_pos++] = '\0';
    writer_->AddString(buffer.start());
  }
}


void HeapSnapshotJSONSerializer::SerializeSamples() {
  const std::vector<HeapObjectsMap::TimeInterval>& samples =
      snapshot_->profiler()->heap_object_map()->samples();
  if (samples.empty()) return;
  base::TimeTicks start_time = samples[0].timestamp;
  // The buffer needs space for 2 unsigned ints, 2 commas, \n and \0
  const int kBufferSize = MaxDecimalDigitsIn<sizeof(
                              base::TimeDelta().InMicroseconds())>::kUnsigned +
                          MaxDecimalDigitsIn<sizeof(samples[0].id)>::kUnsigned +
                          2 + 1 + 1;
  EmbeddedVector<char, kBufferSize> buffer;
  int i = 0;
  for (const HeapObjectsMap::TimeInterval& sample : samples) {
    int buffer_pos = 0;
    if (i++ > 0) {
      buffer[buffer_pos++] = ',';
    }
    base::TimeDelta time_delta = sample.timestamp - start_time;
    buffer_pos = utoa(time_delta.InMicroseconds(), buffer, buffer_pos);
    buffer[buffer_pos++] = ',';
    buffer_pos = utoa(sample.last_assigned_id(), buffer, buffer_pos);
    buffer[buffer_pos++] = '\n';
    buffer[buffer_pos++] = '\0';
    writer_->AddString(buffer.start());
  }
}


void HeapSnapshotJSONSerializer::SerializeString(const unsigned char* s) {
  writer_->AddCharacter('\n');
  writer_->AddCharacter('\"');
  for ( ; *s != '\0'; ++s) {
    switch (*s) {
      case '\b':
        writer_->AddString("\\b");
        continue;
      case '\f':
        writer_->AddString("\\f");
        continue;
      case '\n':
        writer_->AddString("\\n");
        continue;
      case '\r':
        writer_->AddString("\\r");
        continue;
      case '\t':
        writer_->AddString("\\t");
        continue;
      case '\"':
      case '\\':
        writer_->AddCharacter('\\');
        writer_->AddCharacter(*s);
        continue;
      default:
        if (*s > 31 && *s < 128) {
          writer_->AddCharacter(*s);
        } else if (*s <= 31) {
          // Special character with no dedicated literal.
          WriteUChar(writer_, *s);
        } else {
          // Convert UTF-8 into \u UTF-16 literal.
          size_t length = 1, cursor = 0;
          for ( ; length <= 4 && *(s + length) != '\0'; ++length) { }
          unibrow::uchar c = unibrow::Utf8::CalculateValue(s, length, &cursor);
          if (c != unibrow::Utf8::kBadChar) {
            WriteUChar(writer_, c);
            DCHECK_NE(cursor, 0);
            s += cursor - 1;
          } else {
            writer_->AddCharacter('?');
          }
        }
    }
  }
  writer_->AddCharacter('\"');
}


void HeapSnapshotJSONSerializer::SerializeStrings() {
  ScopedVector<const unsigned char*> sorted_strings(
      strings_.occupancy() + 1);
  for (base::HashMap::Entry* entry = strings_.Start(); entry != nullptr;
       entry = strings_.Next(entry)) {
    int index = static_cast<int>(reinterpret_cast<uintptr_t>(entry->value));
    sorted_strings[index] = reinterpret_cast<const unsigned char*>(entry->key);
  }
  writer_->AddString("\"<dummy>\"");
  for (int i = 1; i < sorted_strings.length(); ++i) {
    writer_->AddCharacter(',');
    SerializeString(sorted_strings[i]);
    if (writer_->aborted()) return;
  }
}


}  // namespace internal
}  // namespace v8
