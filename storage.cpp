/**
 * Master thesis
 * by Alf-Andre Walla 2016-2017
 *
**/
#include "storage.hpp"

#include <kernel/os.hpp>
#include <util/crc32.hpp>
#include <cassert>
//#define VERIFY_MEMORY

const uint64_t storage_header::LIVEUPD_MAGIC = 0xbaadb33fdeadc0de;

storage_header::storage_header()
  : magic(LIVEUPD_MAGIC), crc(0), entries(0), length(0)
{
  //printf("%p --> %#llx\n", this, value);
}

void storage_header::add_marker(uint16_t id)
{
  create_entry(TYPE_MARKER, id, 0);
}
void storage_header::add_int(uint16_t id, int value)
{
  create_entry(TYPE_INTEGER, id, value);
}
void storage_header::add_string(uint16_t id, const std::string& data)
{
  auto& entry = create_entry(TYPE_STRING, id, data.size());
  /// copy string (but not the zero)
  memcpy(entry.vla, data.c_str(), data.size());
#ifdef VERIFY_MEMORY
  /// verify memory
  uint32_t csum = crc32(data.c_str(), data.size());
  assert(entry.checksum() == csum);
#endif
}
void storage_header::add_buffer(uint16_t id, const char* buffer, int length)
{
  auto& entry = create_entry(TYPE_BUFFER, id, length);
  memcpy(entry.vla, buffer, length);
#ifdef VERIFY_MEMORY
  /// verify memory
  uint32_t csum = crc32(buffer, length);
  assert(entry.checksum() == csum);
#endif
}
storage_entry& storage_header::add_struct(int16_t type, uint16_t id, int length)
{
  return create_entry(type, id, length);
}
storage_entry& storage_header::add_struct(int16_t type, uint16_t id, construct_func func)
{
  return var_entry(type, id, func);
}
void storage_header::add_vector(uint16_t id, const void* buf, size_t cnt, size_t esize)
{
  auto& entry = create_entry(TYPE_VECTOR, id, sizeof(segmented_entry) + cnt * esize);
  auto& segs = entry.get_segs();
  segs.count = cnt;
  segs.esize = esize;
  memcpy(segs.vla, buf, segs.count * segs.esize);
  /// TODO: verify, but keep in mind segmented_entry is not part of (buf, cnt*esize)
}
void storage_header::add_string_vector(uint16_t id, const std::vector<std::string>& vec)
{
  var_entry(TYPE_STR_VECTOR, id,
  [&vec] (char* dest) -> int
  {
    int total_len = sizeof(varseg_begin);
    // header containing count
    auto* head = (varseg_begin*) dest;
    head->count = vec.size();
    // each element with entry header
    auto* el = (varseg_entry*) head->vla;
    for (auto& str : vec)
    {
      el->len = str.size();
      memcpy(el->vla, str.data(), el->len);
      total_len += sizeof(varseg_entry) + el->len;
      // next
      el = (varseg_entry*) &el->vla[el->len];
    }
    return total_len;
  });
}
void storage_header::add_end()
{
  auto& ent = create_entry(TYPE_END);

  // test against heap max
  uintptr_t storage_end = (uintptr_t) ent.vla;
  if (storage_end > OS::heap_max())
  {
    printf("ERROR:\n"
          "Storage end outside memory: %#lx > %#lx by %ld bytes\n",
	        storage_end,
	        OS::heap_max()+1,
          storage_end - (OS::heap_max()+1));
    throw std::runtime_error("LiveUpdate storage end outside memory");
  }
  // verify memory is writable at the current end
  static const int END_CANARY = 0xbeefc4f3;
  *((volatile int*) &ent.len) = END_CANARY;
  if (ent.len != END_CANARY)
      throw std::runtime_error("Failed to write canary to end of storage");
  // restore length to zero
  ent.len = 0;
}

void storage_header::finalize()
{
  if (this->magic != LIVEUPD_MAGIC)
      throw std::runtime_error("Magic field invalidated during store process");
  add_end();
  this->crc = generate_checksum();
}
bool storage_header::validate() noexcept
{
  if (this->magic != LIVEUPD_MAGIC) return false;
  if (this->crc   == 0) return false;

  uint32_t chsum = generate_checksum();
  if (this->crc != chsum) return false;
  return true;
}

uint32_t storage_header::generate_checksum() noexcept
{
  uint32_t crc_copy = this->crc;
  this->crc         = 0;

  const char* begin = (const char*) this;
  size_t      len   = sizeof(storage_header) + this->length;
  uint32_t checksum = crc32(begin, len);

  this->crc = crc_copy;
  return checksum;
}

void storage_header::zero()
{
  memset(this, 0, sizeof(storage_header) + this->length);
  assert(this->magic == 0);
}

storage_entry* storage_header::begin()
{
  return (storage_entry*) vla;
}
storage_entry* storage_header::next(storage_entry* ptr)
{
  assert(ptr);
  return ptr->next();
}

storage_entry::storage_entry(int16_t t, uint16_t ID, int v)
  : type(t), id(ID), len(v) {}
// used for last entry, for the most part
storage_entry::storage_entry(int16_t t)
  : type(t), id(0), len(0)  {}

storage_entry* storage_entry::next() const noexcept
{
  assert(type != TYPE_END && "Storage entry END cannot have next entry");
  return (storage_entry*) &vla[length()];
}

uint32_t storage_entry::checksum() const
{
  return crc32(vla, length());
}
