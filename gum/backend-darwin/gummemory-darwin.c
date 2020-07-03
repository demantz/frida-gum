/*
 * Copyright (C) 2010-2020 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gummemory.h"

#include "gumdarwin.h"
#include "gummemory-priv.h"

#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

typedef gboolean (* GumFoundFreeRangeFunc) (const GumMemoryRange * range,
    gpointer user_data);

typedef struct _GumAllocNearContext GumAllocNearContext;

struct _GumAllocNearContext
{
  gpointer result;
  gsize size;
  const GumAddressSpec * address_spec;
  mach_port_t task;
};

static guint8 * gum_darwin_read_modern (mach_port_t task, GumAddress address,
    gsize len, gsize * n_bytes_read);
static guint8 * gum_darwin_read_legacy (mach_port_t task, GumAddress address,
    gsize len, gsize * n_bytes_read);
static gboolean gum_try_alloc_in_range_if_near_enough (
    const GumMemoryRange * range, gpointer user_data);
static gpointer gum_allocate_page_aligned (gpointer address, gsize size,
    gint prot);
static gint gum_page_protection_to_bsd (GumPageProtection page_prot);

void
_gum_memory_backend_init (void)
{
}

void
_gum_memory_backend_deinit (void)
{
}

guint
_gum_memory_backend_query_page_size (void)
{
  return getpagesize ();
}

gboolean
gum_darwin_query_ptrauth_support (mach_port_t task,
                                  GumPtrauthSupport * ptrauth_support)
{
#if defined (HAVE_IOS) && (defined (HAVE_ARM) || defined (HAVE_ARM64))
  GumDarwinAllImageInfos infos;
  GumAddress actual_ptr, stripped_ptr;

  if (task == mach_task_self ())
  {
    *ptrauth_support = gum_query_ptrauth_support ();
    return TRUE;
  }

  if (!gum_darwin_query_all_image_infos (task, &infos))
    return FALSE;

  actual_ptr = infos.notification_address;
  stripped_ptr = actual_ptr & G_GUINT64_CONSTANT (0x7fffffffff);

  *ptrauth_support = (stripped_ptr != actual_ptr)
      ? GUM_PTRAUTH_SUPPORTED
      : GUM_PTRAUTH_UNSUPPORTED;
#else
  *ptrauth_support = GUM_PTRAUTH_UNSUPPORTED;
#endif

  return TRUE;
}

gboolean
gum_darwin_query_page_size (mach_port_t task,
                            guint * page_size)
{
  int pid;
  kern_return_t kr;
  GumCpuType cpu_type;

  if (task == mach_task_self ())
  {
    *page_size = gum_query_page_size ();
    return TRUE;
  }

  /* FIXME: any way we can probe it without access to the task's host port? */
  kr = pid_for_task (task, &pid);
  if (kr != KERN_SUCCESS)
    return FALSE;

  if (!gum_darwin_cpu_type_from_pid (pid, &cpu_type))
    return FALSE;

  switch (cpu_type)
  {
    case GUM_CPU_IA32:
    case GUM_CPU_AMD64:
      *page_size = 4096;
      break;
    case GUM_CPU_ARM:
    {
      if (gum_darwin_check_xnu_version (3216, 0, 0))
      {
        char buf[256];
        size_t size;
        int res;
        guint64 hw_page_size = 0;

        size = sizeof (buf);
        res = sysctlbyname ("hw.pagesize", buf, &size, NULL, 0);
        g_assert (res == 0);

        if (size == 8)
          hw_page_size = *((guint64 *) buf);
        else if (size == 4)
          hw_page_size = *((guint32 *) buf);
        else
          g_assert_not_reached ();

        *page_size = hw_page_size;
      }
      else
      {
        *page_size = 4096;
      }

      break;
    }
    case GUM_CPU_ARM64:
      *page_size = 16384;
      break;
  }

  return TRUE;
}

static void
gum_memory_enumerate_free_ranges (GumFoundFreeRangeFunc func,
                                  gpointer user_data)
{
  mach_port_t self;
  guint page_size, index;
  mach_vm_address_t address = MACH_VM_MIN_ADDRESS;
  GumAddress prev_end = 0;

  self = mach_task_self ();

  page_size = gum_query_page_size ();

  for (index = 0; TRUE; index++)
  {
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr;

    kr = mach_vm_region_recurse (self, &address, &size, &depth,
        (vm_region_recurse_info_t) &info, &info_count);
    if (kr != KERN_SUCCESS)
    {
      if (prev_end != 0)
      {
        GumAddress max_address;
        GumMemoryRange r;

#if GLIB_SIZEOF_VOID_P == 4
        max_address = 0xffffffff;
#elif defined (HAVE_I386)
        max_address = G_GUINT64_CONSTANT (0x0001000000000000);
#elif defined (HAVE_ARM64)
        max_address = G_GUINT64_CONSTANT (0x0000000200000000);
#endif

        if (max_address > prev_end)
        {
          r.base_address = prev_end;
          r.size = max_address - prev_end;

          func (&r, user_data);
        }
      }

      break;
    }

    if (index == 0 && address > page_size)
    {
      GumMemoryRange r;

      r.base_address = page_size;
      r.size = address - page_size;

      if (!func (&r, user_data))
        break;
    }

    if (prev_end != 0)
    {
      gint64 gap_size;

      gap_size = address - prev_end;

      if (gap_size > 0)
      {
        GumMemoryRange r;

        r.base_address = prev_end;
        r.size = gap_size;

        if (!func (&r, user_data))
          break;
      }
    }

    prev_end = address + size;

    address += size;
  }
}

gboolean
gum_memory_is_readable (gconstpointer address,
                        gsize len)
{
  gboolean is_readable;
  guint8 * bytes;
  gsize n_bytes_read;

  bytes = gum_memory_read (address, len, &n_bytes_read);
  is_readable = bytes != NULL && n_bytes_read == len;
  g_free (bytes);

  return is_readable;
}

guint8 *
gum_memory_read (gconstpointer address,
                 gsize len,
                 gsize * n_bytes_read)
{
  return gum_darwin_read (mach_task_self (), GUM_ADDRESS (address), len,
      n_bytes_read);
}

gboolean
gum_memory_write (gpointer address,
                  const guint8 * bytes,
                  gsize len)
{
  return gum_darwin_write (mach_task_self (), GUM_ADDRESS (address), bytes,
      len);
}

guint8 *
gum_darwin_read (mach_port_t task,
                 GumAddress address,
                 gsize len,
                 gsize * n_bytes_read)
{
  return gum_darwin_check_xnu_version (2782, 20, 48)
      ? gum_darwin_read_modern (task, address, len, n_bytes_read)
      : gum_darwin_read_legacy (task, address, len, n_bytes_read);
}

static guint8 *
gum_darwin_read_modern (mach_port_t task,
                        GumAddress address,
                        gsize len,
                        gsize * n_bytes_read)
{
  guint8 * result;
  kern_return_t kr;

  result = g_malloc (len);

#ifdef HAVE_IOS
  mach_vm_size_t n;

  /* mach_vm_read() corrupts memory on iOS */
  kr = mach_vm_read_overwrite (task, address, len, (vm_address_t) result, &n);
  if (kr != KERN_SUCCESS)
    goto invalid_address;
#else
  vm_offset_t buf;
  mach_msg_type_number_t n;

  /* mach_vm_read_overwrite() leaks memory on macOS */
  kr = mach_vm_read (task, address, len, &buf, &n);
  if (kr != KERN_SUCCESS)
    goto invalid_address;
  memcpy (result, GSIZE_TO_POINTER (buf), n);
  mach_vm_deallocate (mach_task_self (), buf, n);
#endif

  if (n_bytes_read != NULL)
    *n_bytes_read = n;

  return result;

invalid_address:
  {
    g_free (result);

    return NULL;
  }
}

static guint8 *
gum_darwin_read_legacy (mach_port_t task,
                        GumAddress address,
                        gsize len,
                        gsize * n_bytes_read)
{
  guint page_size;
  guint8 * result;
  gsize offset;
  mach_port_t self;
  kern_return_t kr;

  if (!gum_darwin_query_page_size (task, &page_size))
    return NULL;

  result = g_malloc (len);
  offset = 0;

  self = mach_task_self ();

  while (offset != len)
  {
    GumAddress chunk_address, page_address;
    gsize chunk_size, page_offset;

    /*
     * Read one page at a time to avoid crashing the kernel (CVE-2015-1141).
     *
     * Fixed in XNU 2782.20.48, shipped with macOS 10.10.3:
     * https://support.apple.com/en-us/HT204659
     */
    chunk_address = address + offset;
    page_address = chunk_address & ~(GumAddress) (page_size - 1);
    page_offset = chunk_address - page_address;
    chunk_size = MIN (len - offset, page_size - page_offset);

#ifdef HAVE_IOS
    mach_vm_size_t n;

    /* mach_vm_read corrupts() memory on iOS */
    kr = mach_vm_read_overwrite (task, chunk_address, chunk_size,
        (vm_address_t) (result + offset), &n);
    if (kr != KERN_SUCCESS)
      break;
    g_assert (n == chunk_size);
#else
    vm_offset_t buf;
    mach_msg_type_number_t n;

    /* mach_vm_read_overwrite() leaks memory on macOS */
    kr = mach_vm_read (task, page_address, page_size, &buf, &n);
    if (kr != KERN_SUCCESS)
      break;
    g_assert (n == page_size);
    memcpy (result + offset, (gpointer) (buf + page_offset),
        chunk_size);
    mach_vm_deallocate (self, buf, n);
#endif

    offset += chunk_size;
  }

  if (offset == 0)
  {
    g_free (result);
    result = NULL;
  }

  if (n_bytes_read != NULL)
    *n_bytes_read = offset;

  return result;
}

gboolean
gum_darwin_write (mach_port_t task,
                  GumAddress address,
                  const guint8 * bytes,
                  gsize len)
{
  kern_return_t kr;

  kr = mach_vm_write (task, address, (vm_offset_t) bytes, len);

  return (kr == KERN_SUCCESS);
}

static kern_return_t
gum_mach_vm_protect (vm_map_t target_task,
                     mach_vm_address_t address,
                     mach_vm_size_t size,
                     boolean_t set_maximum,
                     vm_prot_t new_protection)
{
#if defined (HAVE_ARM)
  kern_return_t result;
  guint32 args[] = {
    target_task,
    address & 0xffffffff,
    (address >> 32) & 0xffffffff,
    size & 0xffffffff,
    (size >> 32) & 0xffffffff,
    set_maximum,
    new_protection,
    0
  };

  /* FIXME: Should avoid clobbering R7, which is reserved. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winline-asm"

  asm volatile (
      "push {r0, r1, r2, r3, r4, r5, r6, r7, r12}\n\t"
      "ldmdb %1!, {r0, r1, r2, r3, r4, r5, r6, r7}\n\t"
      "mvn r12, 0xd\n\t"
      "svc 0x80\n\t"
      "mov %0, r0\n\t"
      "pop {r0, r1, r2, r3, r4, r5, r6, r7, r12}\n\t"
      : "=r" (result)
      : "r" (args + G_N_ELEMENTS (args))
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r12"
  );

#pragma clang diagnostic pop

  return result;
#elif defined (HAVE_ARM64)
  kern_return_t result;

  asm volatile (
      "sub sp, sp, #16 * 3\n\t"
      "stp x0, x1, [sp, #16 * 0]\n\t"
      "stp x2, x3, [sp, #16 * 1]\n\t"
      "stp x4, x16, [sp, #16 * 2]\n\t"
      "mov x0, %1\n\t"
      "mov x1, %2\n\t"
      "mov x2, %3\n\t"
      "mov x3, %4\n\t"
      "mov x4, %5\n\t"
      "movn x16, 0xd\n\t"
      "svc 0x80\n\t"
      "mov %w0, w0\n\t"
      "ldp x0, x1, [sp, #16 * 0]\n\t"
      "ldp x2, x3, [sp, #16 * 1]\n\t"
      "ldp x4, x16, [sp, #16 * 2]\n\t"
      "add sp, sp, #16 * 3\n\t"
      : "=r" (result)
      : "r" ((gsize) target_task),
        "r" (address),
        "r" (size),
        "r" ((gsize) set_maximum),
        "r" ((gsize) new_protection)
      : "x0", "x1", "x2", "x3", "x4", "x16"
  );

  return result;
#else
  return mach_vm_protect (target_task, address, size, set_maximum,
      new_protection);
#endif
}

gboolean
gum_try_mprotect (gpointer address,
                  gsize size,
                  GumPageProtection page_prot)
{
  gsize page_size;
  gpointer aligned_address;
  gsize aligned_size;
  vm_prot_t mach_page_prot;
  kern_return_t kr;

  g_assert (size != 0);

  page_size = gum_query_page_size ();
  aligned_address = GSIZE_TO_POINTER (
      GPOINTER_TO_SIZE (address) & ~(page_size - 1));
  aligned_size =
      (1 + ((address + size - 1 - aligned_address) / page_size)) * page_size;
  mach_page_prot = gum_page_protection_to_mach (page_prot);

  kr = gum_mach_vm_protect (mach_task_self (),
      GPOINTER_TO_SIZE (aligned_address), aligned_size, FALSE, mach_page_prot);

  return kr == KERN_SUCCESS;
}

void
gum_clear_cache (gpointer address,
                 gsize size)
{
  sys_icache_invalidate (address, size);
  sys_dcache_flush (address, size);
}

gpointer
gum_try_alloc_n_pages (guint n_pages,
                       GumPageProtection page_prot)
{
  mach_vm_address_t result = 0;
  gsize page_size, size;
  kern_return_t kr;

  page_size = gum_query_page_size ();
  size = (1 + n_pages) * page_size;

  kr = mach_vm_allocate (mach_task_self (), &result, size, VM_FLAGS_ANYWHERE);
  if (kr != KERN_SUCCESS)
    return NULL;

  *((gsize *) GSIZE_TO_POINTER (result)) = size;

  gum_mprotect (GSIZE_TO_POINTER (result), page_size, GUM_PAGE_READ);

  if (page_prot != GUM_PAGE_RW)
  {
    gum_mprotect (GSIZE_TO_POINTER (result + page_size), size - page_size,
        page_prot);
  }

  return GSIZE_TO_POINTER (result + page_size);
}

gpointer
gum_try_alloc_n_pages_near (guint n_pages,
                            GumPageProtection page_prot,
                            const GumAddressSpec * address_spec)
{
  gsize page_size;
  GumAllocNearContext ctx;

  page_size = gum_query_page_size ();

  ctx.result = NULL;
  ctx.size = (1 + n_pages) * gum_query_page_size ();
  ctx.address_spec = address_spec;
  ctx.task = mach_task_self ();

  gum_memory_enumerate_free_ranges (gum_try_alloc_in_range_if_near_enough,
      &ctx);
  if (ctx.result == NULL)
    return NULL;

  *((gsize *) ctx.result) = ctx.size;

  if (page_prot != GUM_PAGE_READ)
  {
    gum_mprotect (ctx.result, page_size, GUM_PAGE_READ);
  }

  if (page_prot != GUM_PAGE_RW)
  {
    gum_mprotect (ctx.result + page_size, ctx.size - page_size, page_prot);
  }

  return ctx.result + page_size;
}

static gboolean
gum_try_alloc_in_range_if_near_enough (const GumMemoryRange * range,
                                       gpointer user_data)
{
  GumAllocNearContext * ctx = user_data;
  GumAddress base_address;
  gsize distance;
  mach_vm_address_t address;
  kern_return_t kr;

  if (range->size < ctx->size)
    return TRUE;

  base_address = range->base_address;
  distance =
      ABS (ctx->address_spec->near_address - GSIZE_TO_POINTER (base_address));
  if (distance > ctx->address_spec->max_distance)
  {
    base_address = range->base_address + range->size - ctx->size;
    distance =
        ABS (ctx->address_spec->near_address - GSIZE_TO_POINTER (base_address));
  }

  if (distance > ctx->address_spec->max_distance)
    return TRUE;

  address = base_address;
  kr = mach_vm_allocate (ctx->task, &address, ctx->size, VM_FLAGS_FIXED);
  if (kr != KERN_SUCCESS)
    return TRUE;

  ctx->result = GSIZE_TO_POINTER (address);
  return FALSE;
}

void
gum_query_page_allocation_range (gconstpointer mem,
                                 guint size,
                                 GumMemoryRange * range)
{
  gsize page_size = gum_query_page_size ();

  range->base_address = GUM_ADDRESS (mem - page_size);
  range->size = size + page_size;
}

void
gum_free_pages (gpointer mem)
{
  gsize page_size;
  mach_vm_address_t address;
  mach_vm_size_t size;
  kern_return_t kr;

  page_size = gum_query_page_size ();

  address = GPOINTER_TO_SIZE (mem) - page_size;
  size = *((gsize *) address);

  kr = mach_vm_deallocate (mach_task_self (), address, size);
  g_assert (kr == KERN_SUCCESS);
}

gpointer
gum_memory_allocate (gpointer address,
                     gsize size,
                     gsize alignment,
                     GumPageProtection page_prot)
{
  gsize page_size, allocation_size;
  guint8 * base, * aligned_base;

  address = GUM_ALIGN_POINTER (gpointer, address, alignment);

  page_size = gum_query_page_size ();
  allocation_size = size + (alignment - page_size);
  allocation_size = GUM_ALIGN_SIZE (allocation_size, page_size);

  base = gum_allocate_page_aligned (address, allocation_size,
      gum_page_protection_to_bsd (page_prot));
  if (base == NULL)
    return NULL;

  aligned_base = GUM_ALIGN_POINTER (guint8 *, base, alignment);

  if (aligned_base != base)
  {
    gsize prefix_size = aligned_base - base;
    gum_memory_free (base, prefix_size);
    allocation_size -= prefix_size;
  }

  if (allocation_size != size)
  {
    gsize suffix_size = allocation_size - size;
    gum_memory_free (aligned_base + size, suffix_size);
    allocation_size -= suffix_size;
  }

  g_assert (allocation_size == size);

  return aligned_base;
}

static gpointer
gum_allocate_page_aligned (gpointer address,
                           gsize size,
                           gint prot)
{
  gpointer result;

  result = mmap (address, size, prot, MAP_PRIVATE | MAP_ANONYMOUS,
      VM_MAKE_TAG (255), 0);
  if (result == MAP_FAILED)
    return NULL;

#ifdef HAVE_IOS
  {
    gboolean need_checkra1n_quirk;

    need_checkra1n_quirk = prot == (PROT_READ | PROT_WRITE | PROT_EXEC) &&
        gum_query_rwx_support () == GUM_RWX_ALLOCATIONS_ONLY;
    if (need_checkra1n_quirk)
    {
      gum_mach_vm_protect (mach_task_self (), GPOINTER_TO_SIZE (result), size,
          FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    }
  }
#endif

  return result;
}

gboolean
gum_memory_free (gpointer address,
                 gsize size)
{
  return munmap (address, size) == 0;
}

gboolean
gum_memory_release (gpointer address,
                    gsize size)
{
  return gum_memory_free (address, size);
}

gboolean
gum_memory_commit (gpointer address,
                   gsize size,
                   GumPageProtection page_prot)
{
  return madvise (address, size, MADV_FREE_REUSE) == 0;
}

gboolean
gum_memory_decommit (gpointer address,
                     gsize size)
{
  return madvise (address, size, MADV_FREE_REUSABLE) == 0;
}

GumPageProtection
gum_page_protection_from_mach (vm_prot_t native_prot)
{
  GumPageProtection page_prot = 0;

  if ((native_prot & VM_PROT_READ) == VM_PROT_READ)
    page_prot |= GUM_PAGE_READ;
  if ((native_prot & VM_PROT_WRITE) == VM_PROT_WRITE)
    page_prot |= GUM_PAGE_WRITE;
  if ((native_prot & VM_PROT_EXECUTE) == VM_PROT_EXECUTE)
    page_prot |= GUM_PAGE_EXECUTE;

  return page_prot;
}

vm_prot_t
gum_page_protection_to_mach (GumPageProtection page_prot)
{
  vm_prot_t mach_page_prot = VM_PROT_NONE;

  if ((page_prot & GUM_PAGE_READ) != 0)
    mach_page_prot |= VM_PROT_READ;
  if ((page_prot & GUM_PAGE_WRITE) != 0)
    mach_page_prot |= VM_PROT_WRITE | VM_PROT_COPY;
  if ((page_prot & GUM_PAGE_EXECUTE) != 0)
    mach_page_prot |= VM_PROT_EXECUTE;

  return mach_page_prot;
}

static gint
gum_page_protection_to_bsd (GumPageProtection page_prot)
{
  gint posix_page_prot = PROT_NONE;

  if ((page_prot & GUM_PAGE_READ) != 0)
    posix_page_prot |= PROT_READ;
  if ((page_prot & GUM_PAGE_WRITE) != 0)
    posix_page_prot |= PROT_WRITE;
  if ((page_prot & GUM_PAGE_EXECUTE) != 0)
    posix_page_prot |= PROT_EXEC;

  return posix_page_prot;
}
