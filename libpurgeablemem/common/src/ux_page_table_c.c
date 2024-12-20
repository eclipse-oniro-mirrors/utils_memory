/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h> /* NULL */
#include <sys/mman.h> /* mmap */
#include <sched.h> /* sched_yield() */
#include <limits.h>

#include "hilog/log_c.h"
#include "pm_util.h"
#include "ux_page_table_c.h"

#undef LOG_TAG
#define LOG_TAG "PurgeableMemC: UPT"

#if defined(USE_UXPT) && (USE_UXPT > 0)  /* (USE_UXPT > 0) means using uxpt */

/*
 * using uint64_t as uxpte_t to avoid avoid confusion on 32-bit and 64 bit systems.
 * Type uxpte_t may be modified to uint32_t in the future, so typedef is used.
 */
typedef uint64_t uxpte_t;

typedef struct UserExtendPageTable {
    uint64_t dataAddr;
    size_t dataSize;
    uxpte_t *uxpte;
} UxPageTableStruct;

static bool g_supportUxpt = false;

/*
 * -------------------------------------------------------------------------
 * |         virtual page number                |                           |
 * |--------------------------------------------| vaddr offset in virt page |
 * | uxpte page number |  offset in uxpte page  |                           |
 * --------------------------------------------------------------------------
 * |                   |  UXPTE_PER_PAGE_SHIFT  |        PAGE_SHIFT         |
 */
static const size_t UXPTE_SIZE_SHIFT = 3;
static const size_t UXPTE_PER_PAGE_SHIFT = PAGE_SHIFT - UXPTE_SIZE_SHIFT;
static const size_t UXPTE_PER_PAGE = 1 << UXPTE_PER_PAGE_SHIFT;

/* get virtual page number from virtual address */
static inline uint64_t VirtPageNo(uint64_t vaddr)
{
    return vaddr >> PAGE_SHIFT;
}

/* page number in user page table of uxpte for virtual address */
static inline uint64_t UxptePageNo(uint64_t vaddr)
{
    return VirtPageNo(vaddr) >> UXPTE_PER_PAGE_SHIFT;
}

/* uxpte offset in uxpte page for virtual address */
static inline uint64_t UxpteOffset(uint64_t vaddr)
{
    return VirtPageNo(vaddr) & (UXPTE_PER_PAGE - 1);
}

static const size_t UXPTE_PRESENT_BIT = 1;
static const size_t UXPTE_PRESENT_MASK = (1 << UXPTE_PRESENT_BIT) - 1;
static const size_t UXPTE_REFCNT_ONE = 1 << UXPTE_PRESENT_BIT;
static const uxpte_t UXPTE_UNDER_RECLAIM = (uxpte_t)(-UXPTE_REFCNT_ONE);

static inline bool IsUxptePresent(uxpte_t pte)
{
    return pte & (uxpte_t)UXPTE_PRESENT_MASK;
}

static inline bool IsUxpteUnderReclaim(uxpte_t pte)
{
    return pte == UXPTE_UNDER_RECLAIM;
}

static size_t GetUxPageSize(uint64_t dataAddr, size_t dataSize)
{
    if (dataAddr + dataSize < dataAddr || dataAddr + dataSize < dataSize || dataAddr + dataSize < 1) {
        HILOG_ERROR(LOG_CORE, "%{public}s: Addition overflow!", __func__);
        return 0;
    }
    uint64_t pageNoEnd = UxptePageNo(dataAddr + dataSize -1);
    uint64_t pageNoStart = UxptePageNo(dataAddr);
    if (pageNoEnd < pageNoStart) {
        HILOG_ERROR(LOG_CORE, "pageNoEnd < pageNoStart");
        return 0;
    }
    if (pageNoEnd - pageNoStart + 1 > SIZE_MAX / PAGE_SIZE) {
        HILOG_ERROR(LOG_CORE, "pageNoEnd - pageNoStart + 1 > SIZE_MAX / PAGE_SIZE");
        return 0;
    }
    return (pageNoEnd - pageNoStart + 1) * PAGE_SIZE;
}

static inline uint64_t RoundUp(uint64_t val, size_t align)
{
    if (val + align < val || val + align < align) {
        HILOG_ERROR(LOG_CORE, "%{public}s: Addition overflow!", __func__);
        return val;
    }
    if (align == 0) {
        return val;
    }
    return ((val + align - 1) / align) * align;
}

static inline uint64_t RoundDown(uint64_t val, size_t align)
{
    if (align == 0) {
        return val;
    }
    return val & (~(align - 1));
}

enum UxpteOp {
    UPT_GET = 0,
    UPT_PUT = 1,
    UPT_CLEAR = 2,
    UPT_IS_PRESENT = 3,
};

static void __attribute__((constructor)) CheckUxpt(void);
static void UxpteAdd(uxpte_t *pte, size_t incNum);
static void UxpteSub(uxpte_t *pte, size_t decNum);

static void GetUxpteAt(UxPageTableStruct *upt, uint64_t addr);
static void PutUxpteAt(UxPageTableStruct *upt, uint64_t addr);
static bool IsPresentAt(UxPageTableStruct *upt, uint64_t addr);
static PMState UxpteOps(UxPageTableStruct *upt, uint64_t addr, size_t len, enum UxpteOp op);

static uxpte_t *MapUxptePages(uint64_t dataAddr, size_t dataSize);
static int UnmapUxptePages(uxpte_t *ptes, size_t size);

static void __attribute__((constructor)) CheckUxpt(void)
{
    int prot = PROT_READ | PROT_WRITE;
    int type = MAP_ANONYMOUS | MAP_PURGEABLE;
    size_t dataSize = PAGE_SIZE;
    /* try to mmap purgable page */
    void *dataPtr = mmap(NULL, dataSize, prot, type, -1, 0);
    if (dataPtr == MAP_FAILED) {
        HILOG_ERROR(LOG_CORE, "%{public}s: not support MAP_PURG", __func__);
        g_supportUxpt = false;
        return;
    }
    /* try to mmap uxpt page */
    type = MAP_ANONYMOUS | MAP_USEREXPTE;
    size_t uptSize = GetUxPageSize((uint64_t)dataPtr, dataSize);
    void *ptes = mmap(NULL, uptSize, prot, type, -1, UxptePageNo((uint64_t)dataPtr) * PAGE_SIZE);
    if (ptes != MAP_FAILED) {
        g_supportUxpt = true;
        /* free uxpt */
        if (munmap(ptes, uptSize) != 0) {
            HILOG_ERROR(LOG_CORE, "%{public}s: unmap uxpt fail", __func__);
        }
    } else { /* MAP_FAILED */
        g_supportUxpt = false;
        HILOG_ERROR(LOG_CORE, "%{public}s: not support uxpt", __func__);
    }
    ptes = NULL;
    /* free data */
    if (munmap(dataPtr, dataSize) != 0) {
        HILOG_ERROR(LOG_CORE, "%{public}s: unmap purg data fail", __func__);
    }
    dataPtr = NULL;
    HILOG_INFO(LOG_CORE, "%{public}s: supportUxpt=%{public}s", __func__, (g_supportUxpt ? "1" : "0"));
    return;
}

bool UxpteIsEnabled(void)
{
    return g_supportUxpt;
}

size_t UxPageTableSize(void)
{
    return sizeof(UxPageTableStruct);
}

PMState InitUxPageTable(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    if (!g_supportUxpt) {
        HILOG_DEBUG(LOG_CORE, "%{public}s: not support uxpt", __func__);
        return PM_OK;
    }
    if (upt == NULL) {
        HILOG_ERROR(LOG_CORE, "%{public}s: upt is NULL!", __func__);
        return PM_MMAP_UXPT_FAIL;
    }
    upt->dataAddr = addr;
    upt->dataSize = len;
    upt->uxpte = MapUxptePages(upt->dataAddr, upt->dataSize);
    if (!(upt->uxpte)) {
        return PM_MMAP_UXPT_FAIL;
    }
    UxpteClear(upt, addr, len);
    return PM_OK;
}

PMState DeinitUxPageTable(UxPageTableStruct *upt)
{
    if (!g_supportUxpt) {
        HILOG_DEBUG(LOG_CORE, "%{public}s: not support uxpt", __func__);
        return PM_OK;
    }
    if (upt == NULL) {
        HILOG_ERROR(LOG_CORE, "%{public}s: upt is NULL!", __func__);
        return PM_MMAP_UXPT_FAIL;
    }
    size_t size = GetUxPageSize(upt->dataAddr, upt->dataSize);
    int unmapRet = 0;
    if (upt->uxpte) {
        unmapRet = UnmapUxptePages(upt->uxpte, size);
        if (unmapRet != 0) {
            HILOG_ERROR(LOG_CORE, "%{public}s: unmap uxpt fail", __func__);
            return PM_UNMAP_UXPT_FAIL;
        }
        upt->uxpte = NULL;
    }
    upt->dataAddr = 0;
    upt->dataSize = 0;
    return PM_OK;
}

void UxpteGet(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    if (!g_supportUxpt) {
        return;
    }
    UxpteOps(upt, addr, len, UPT_GET);
}

void UxptePut(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    if (!g_supportUxpt) {
        return;
    }
    UxpteOps(upt, addr, len, UPT_PUT);
}

void UxpteClear(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    if (!g_supportUxpt) {
        return;
    }
    UxpteOps(upt, addr, len, UPT_CLEAR);
}

bool UxpteIsPresent(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    if (!g_supportUxpt) {
        return true;
    }
    PMState ret = UxpteOps(upt, addr, len, UPT_IS_PRESENT);
    return ret == PM_OK;
}

static inline uxpte_t UxpteLoad(const uxpte_t *uxpte)
{
    __sync_synchronize();
    return *uxpte;
}

static inline bool UxpteCAS_(uxpte_t *uxpte, uxpte_t old, uxpte_t newVal)
{
    return __sync_bool_compare_and_swap(uxpte, old, newVal);
}

static void UxpteAdd(uxpte_t *pte, size_t incNum)
{
    uxpte_t old = 0;
    uxpte_t newVal = 0;
    do {
        old = UxpteLoad(pte);
        if (old + incNum < old || old + incNum < incNum) {
            break;
        }
        newVal = old + incNum;
        if (ULONG_MAX - old < incNum) {
            return;
        }
        if (IsUxpteUnderReclaim(old)) {
            sched_yield();
            continue;
        }
    } while (!UxpteCAS_(pte, old, newVal));
}

static void UxpteSub(uxpte_t *pte, size_t decNum)
{
    uxpte_t old;
    do {
        old = UxpteLoad(pte);
    } while (!UxpteCAS_(pte, old, old - decNum));
}

static void UxpteClear_(uxpte_t *pte)
{
    uxpte_t old = UxpteLoad(pte);
    if ((unsigned long long)old == 0) {
        return; /* has been set to zero */
    }
    HILOG_ERROR(LOG_CORE, "%{public}s: upte(0x%{public}llx) != 0", __func__, (unsigned long long)old);
    do {
        old = UxpteLoad(pte);
    } while (!UxpteCAS_(pte, old, 0));
}

static inline size_t GetIndexInUxpte(uint64_t startAddr, uint64_t currAddr)
{
    return UxpteOffset(startAddr) + (VirtPageNo(currAddr) - VirtPageNo(startAddr));
}

static void GetUxpteAt(UxPageTableStruct *upt, uint64_t addr)
{
    if (upt == NULL) {
        HILOG_ERROR(LOG_CORE, "%{public}s: upt is NULL!", __func__);
        return;
    }
    size_t index = GetIndexInUxpte(upt->dataAddr, addr);
    UxpteAdd(&(upt->uxpte[index]), UXPTE_REFCNT_ONE);

    HILOG_DEBUG(LOG_CORE, "%{public}s: addr(0x%{public}llx) upte=0x%{public}llx",
        __func__, (unsigned long long)addr, (unsigned long long)(upt->uxpte[index]));
}

static void PutUxpteAt(UxPageTableStruct *upt, uint64_t addr)
{
    if (upt == NULL) {
        HILOG_ERROR(LOG_CORE, "%{public}s: upt is NULL!", __func__);
        return;
    }
    size_t index = GetIndexInUxpte(upt->dataAddr, addr);
    UxpteSub(&(upt->uxpte[index]), UXPTE_REFCNT_ONE);

    HILOG_DEBUG(LOG_CORE, "%{public}s: addr(0x%{public}llx) upte=0x%{public}llx",
        __func__, (unsigned long long)addr, (unsigned long long)(upt->uxpte[index]));
}

static void ClearUxpteAt(UxPageTableStruct *upt, uint64_t addr)
{
    if (upt == NULL) {
        HILOG_ERROR(LOG_CORE, "%{public}s: upt is NULL!", __func__);
        return;
    }
    size_t index = GetIndexInUxpte(upt->dataAddr, addr);
    UxpteClear_(&(upt->uxpte[index]));
}

static bool IsPresentAt(UxPageTableStruct *upt, uint64_t addr)
{
    size_t index = GetIndexInUxpte(upt->dataAddr, addr);

    HILOG_DEBUG(LOG_CORE, "%{public}s: addr(0x%{public}llx) upte=0x%{public}llx PRESENT_MASK=0x%{public}zx",
        __func__, (unsigned long long)addr, (unsigned long long)(upt->uxpte[index]), UXPTE_PRESENT_MASK);

    return IsUxptePresent(upt->uxpte[index]);
}

static PMState UxpteOps(UxPageTableStruct *upt, uint64_t addr, size_t len, enum UxpteOp op)
{
    if (upt == NULL) {
        return PM_BUILDER_NULL;
    }
    uint64_t start =  RoundDown(addr, PAGE_SIZE);
    uint64_t end = RoundUp(addr + len, PAGE_SIZE);
    if (start < upt->dataAddr || end > (upt->dataAddr + upt->dataSize)) {
        HILOG_ERROR(LOG_CORE, "%{public}s: addr(0x%{public}llx) start(0x%{public}llx) < dataAddr(0x%{public}llx)"
            " || end(0x%{public}llx) > dataAddr+dataSize(0x%{public}llx) out of bound",
            __func__, (unsigned long long)addr, (unsigned long long)start, (unsigned long long)(upt->dataAddr),
            (unsigned long long)end, (unsigned long long)(upt->dataAddr + upt->dataSize));

        return PM_UXPT_OUT_RANGE;
    }

    for (uint64_t off = start; off < end; off += PAGE_SIZE) {
        switch (op) {
            case UPT_GET: {
                GetUxpteAt(upt, off);
                break;
            }
            case UPT_PUT: {
                PutUxpteAt(upt, off);
                break;
            }
            case UPT_CLEAR: {
                ClearUxpteAt(upt, off);
                break;
            }
            case UPT_IS_PRESENT: {
                if (!IsPresentAt(upt, off)) {
                    HILOG_ERROR(LOG_CORE, "%{public}s: addr(0x%{public}llx) not present", __func__,
                        (unsigned long long)addr);
                    return PM_UXPT_NO_PRESENT;
                }
                break;
            }
            default:
                break;
        }
    }

    return PM_OK;
}

static uxpte_t *MapUxptePages(uint64_t dataAddr, size_t dataSize)
{
    int prot = PROT_READ | PROT_WRITE;
    int type = MAP_ANONYMOUS | MAP_USEREXPTE;
    size_t size = GetUxPageSize(dataAddr, dataSize);
    uxpte_t *ptes = (uxpte_t*)mmap(NULL, size, prot, type, -1, UxptePageNo(dataAddr) * PAGE_SIZE);
    if (ptes == MAP_FAILED) {
        HILOG_ERROR(LOG_CORE, "%{public}s: fail, return NULL", __func__);
        ptes = NULL;
    }

    return ptes;
}

static int UnmapUxptePages(uxpte_t *ptes, size_t size)
{
    return munmap(ptes, size);
}

#else /* !(defined(USE_UXPT) && (USE_UXPT <= 0)), it means does not using uxpt */

typedef struct UserExtendPageTable {
    /* i am empty */
} UxPageTableStruct;

bool UxpteIsEnabled(void)
{
    return false;
}

size_t UxPageTableSize(void)
{
    return 0;
}

PMState InitUxPageTable(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    return PM_OK;
}

PMState DeinitUxPageTable(UxPageTableStruct *upt)
{
    return PM_OK;
}

void UxpteGet(UxPageTableStruct *upt, uint64_t addr, size_t len) {}

void UxptePut(UxPageTableStruct *upt, uint64_t addr, size_t len) {}

bool UxpteIsPresent(UxPageTableStruct *upt, uint64_t addr, size_t len)
{
    return true;
}

#endif /* USE_UXPT > 0 */
