/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkStrikeCache.h"

#include <cctype>

#include "include/core/SkGraphics.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTraceMemoryDump.h"
#include "include/core/SkTypeface.h"
#include "include/private/SkMutex.h"
#include "include/private/SkTemplates.h"
#include "src/core/SkGlyphRunPainter.h"
#include "src/core/SkScalerCache.h"

bool gSkUseThreadLocalStrikeCaches_IAcknowledgeThisIsIncrediblyExperimental = false;

SkStrikeCache* SkStrikeCache::GlobalStrikeCache() {
#if !defined(SK_BUILD_FOR_IOS)
    if (gSkUseThreadLocalStrikeCaches_IAcknowledgeThisIsIncrediblyExperimental) {
        static thread_local auto* cache = new SkStrikeCache;
        return cache;
    }
#endif
    static auto* cache = new SkStrikeCache;
    return cache;
}

SkStrikeCache::ExclusiveStrikePtr::ExclusiveStrikePtr(SkStrikeCache::Strike* strike)
    : fStrike{strike} {}

SkStrikeCache::ExclusiveStrikePtr::ExclusiveStrikePtr()
    : fStrike{nullptr} {}

SkStrikeCache::ExclusiveStrikePtr::ExclusiveStrikePtr(ExclusiveStrikePtr&& o)
    : fStrike{o.fStrike} {
    o.fStrike = nullptr;
}

SkStrikeCache::ExclusiveStrikePtr&
SkStrikeCache::ExclusiveStrikePtr::operator = (ExclusiveStrikePtr&& o) {
    if (fStrike != nullptr) {
        fStrike->fStrikeCache->attachStrike(fStrike);
    }
    fStrike = o.fStrike;
    o.fStrike = nullptr;
    return *this;
}

SkStrikeCache::ExclusiveStrikePtr::~ExclusiveStrikePtr() {
    if (fStrike != nullptr) {
        fStrike->fStrikeCache->attachStrike(fStrike);
    }
}

SkStrike* SkStrikeCache::ExclusiveStrikePtr::get() const {
    return fStrike;
}

SkStrike* SkStrikeCache::ExclusiveStrikePtr::operator -> () const {
    return this->get();
}

SkStrike& SkStrikeCache::ExclusiveStrikePtr::operator *  () const {
    return *this->get();
}

SkStrikeCache::ExclusiveStrikePtr::operator bool () const {
    return fStrike != nullptr;
}

bool operator == (const SkStrikeCache::ExclusiveStrikePtr& lhs,
                  const SkStrikeCache::ExclusiveStrikePtr& rhs) {
    return lhs.fStrike == rhs.fStrike;
}

bool operator == (const SkStrikeCache::ExclusiveStrikePtr& lhs, decltype(nullptr)) {
    return lhs.fStrike == nullptr;
}

bool operator == (decltype(nullptr), const SkStrikeCache::ExclusiveStrikePtr& rhs) {
    return nullptr == rhs.fStrike;
}

SkStrikeCache::~SkStrikeCache() {
    Strike* strike = fHead;
    while (strike) {
        Strike* next = strike->fNext;
        strike->unref();
        strike = next;
    }
}

SkExclusiveStrikePtr SkStrikeCache::findOrCreateStrikeExclusive(
        const SkDescriptor& desc, const SkScalerContextEffects& effects, const SkTypeface& typeface)
{
    return SkExclusiveStrikePtr(this->findOrCreateStrike(desc, effects, typeface));
}

auto SkStrikeCache::findOrCreateStrike(const SkDescriptor& desc,
                                       const SkScalerContextEffects& effects,
                                       const SkTypeface& typeface) -> Strike* {
    Strike* strike = this->findAndDetachStrike(desc);
    if (strike == nullptr) {
        auto scaler = typeface.createScalerContext(effects, &desc);
        strike = this->createStrike(desc, std::move(scaler));
    }
    return strike;
}

SkScopedStrikeForGPU SkStrikeCache::findOrCreateScopedStrike(const SkDescriptor& desc,
                                                             const SkScalerContextEffects& effects,
                                                             const SkTypeface& typeface) {
    return SkScopedStrikeForGPU{this->findOrCreateStrike(desc, effects, typeface)};
}

void SkStrikeCache::PurgeAll() {
    GlobalStrikeCache()->purgeAll();
}

void SkStrikeCache::Dump() {
    SkDebugf("GlyphCache [     used    budget ]\n");
    SkDebugf("    bytes  [ %8zu  %8zu ]\n",
             SkGraphics::GetFontCacheUsed(), SkGraphics::GetFontCacheLimit());
    SkDebugf("    count  [ %8zu  %8zu ]\n",
             SkGraphics::GetFontCacheCountUsed(), SkGraphics::GetFontCacheCountLimit());

    int counter = 0;

    auto visitor = [&counter](const Strike& strike) {
        const SkScalerContextRec& rec = strike.fScalerCache.getScalerContext()->getRec();

        SkDebugf("index %d\n", counter);
        SkDebugf("%s", rec.dump().c_str());
        counter += 1;
    };

    GlobalStrikeCache()->forEachStrike(visitor);
}

namespace {
    const char gGlyphCacheDumpName[] = "skia/sk_glyph_cache";
}  // namespace

void SkStrikeCache::DumpMemoryStatistics(SkTraceMemoryDump* dump) {
    dump->dumpNumericValue(gGlyphCacheDumpName, "size", "bytes", SkGraphics::GetFontCacheUsed());
    dump->dumpNumericValue(gGlyphCacheDumpName, "budget_size", "bytes",
                           SkGraphics::GetFontCacheLimit());
    dump->dumpNumericValue(gGlyphCacheDumpName, "glyph_count", "objects",
                           SkGraphics::GetFontCacheCountUsed());
    dump->dumpNumericValue(gGlyphCacheDumpName, "budget_glyph_count", "objects",
                           SkGraphics::GetFontCacheCountLimit());

    if (dump->getRequestedDetails() == SkTraceMemoryDump::kLight_LevelOfDetail) {
        dump->setMemoryBacking(gGlyphCacheDumpName, "malloc", nullptr);
        return;
    }

    auto visitor = [&dump](const Strike& strike) {
        const SkTypeface* face = strike.fScalerCache.getScalerContext()->getTypeface();
        const SkScalerContextRec& rec = strike.fScalerCache.getScalerContext()->getRec();

        SkString fontName;
        face->getFamilyName(&fontName);
        // Replace all special characters with '_'.
        for (size_t index = 0; index < fontName.size(); ++index) {
            if (!std::isalnum(fontName[index])) {
                fontName[index] = '_';
            }
        }

        SkString dumpName = SkStringPrintf(
                "%s/%s_%d/%p", gGlyphCacheDumpName, fontName.c_str(), rec.fFontID, &strike);

        dump->dumpNumericValue(dumpName.c_str(),
                               "size", "bytes", strike.fScalerCache.getMemoryUsed());
        dump->dumpNumericValue(dumpName.c_str(),
                               "glyph_count", "objects",
                               strike.fScalerCache.countCachedGlyphs());
        dump->setMemoryBacking(dumpName.c_str(), "malloc", nullptr);
    };

    GlobalStrikeCache()->forEachStrike(visitor);
}


void SkStrikeCache::attachStrike(Strike* strike) {
    if (strike == nullptr) {
        return;
    }
    SkAutoSpinlock ac(fLock);

    this->validate();
    strike->fScalerCache.validate();

    this->internalAttachToHead(strike);
    this->internalPurge();
}

SkExclusiveStrikePtr SkStrikeCache::findStrikeExclusive(const SkDescriptor& desc) {
    return SkExclusiveStrikePtr(this->findAndDetachStrike(desc));
}

auto SkStrikeCache::findAndDetachStrike(const SkDescriptor& desc) -> Strike* {
    SkAutoSpinlock ac(fLock);

    for (Strike* strike = fHead; strike != nullptr; strike = strike->fNext) {
        if (strike->fScalerCache.getDescriptor() == desc) {
            this->internalDetachStrike(strike);
            return strike;
        }
    }

    return nullptr;
}

SkExclusiveStrikePtr SkStrikeCache::createStrikeExclusive(
        const SkDescriptor& desc,
        std::unique_ptr<SkScalerContext> scaler,
        SkFontMetrics* maybeMetrics,
        std::unique_ptr<SkStrikePinner> pinner)
{
    return SkExclusiveStrikePtr(
            this->createStrike(desc, std::move(scaler), maybeMetrics, std::move(pinner)));
}

auto SkStrikeCache::createStrike(
        const SkDescriptor& desc,
        std::unique_ptr<SkScalerContext> scaler,
        SkFontMetrics* maybeMetrics,
        std::unique_ptr<SkStrikePinner> pinner) -> Strike* {
    return new Strike{this, desc, std::move(scaler), maybeMetrics, std::move(pinner)};
}

void SkStrikeCache::purgeAll() {
    SkAutoSpinlock ac(fLock);
    this->internalPurge(fTotalMemoryUsed);
}

size_t SkStrikeCache::getTotalMemoryUsed() const {
    SkAutoSpinlock ac(fLock);
    return fTotalMemoryUsed;
}

int SkStrikeCache::getCacheCountUsed() const {
    SkAutoSpinlock ac(fLock);
    return fCacheCount;
}

int SkStrikeCache::getCacheCountLimit() const {
    SkAutoSpinlock ac(fLock);
    return fCacheCountLimit;
}

size_t SkStrikeCache::setCacheSizeLimit(size_t newLimit) {
    static const size_t minLimit = 256 * 1024;
    if (newLimit < minLimit) {
        newLimit = minLimit;
    }

    SkAutoSpinlock ac(fLock);

    size_t prevLimit = fCacheSizeLimit;
    fCacheSizeLimit = newLimit;
    this->internalPurge();
    return prevLimit;
}

size_t  SkStrikeCache::getCacheSizeLimit() const {
    SkAutoSpinlock ac(fLock);
    return fCacheSizeLimit;
}

int SkStrikeCache::setCacheCountLimit(int newCount) {
    if (newCount < 0) {
        newCount = 0;
    }

    SkAutoSpinlock ac(fLock);

    int prevCount = fCacheCountLimit;
    fCacheCountLimit = newCount;
    this->internalPurge();
    return prevCount;
}

int SkStrikeCache::getCachePointSizeLimit() const {
    SkAutoSpinlock ac(fLock);
    return fPointSizeLimit;
}

int SkStrikeCache::setCachePointSizeLimit(int newLimit) {
    if (newLimit < 0) {
        newLimit = 0;
    }

    SkAutoSpinlock ac(fLock);

    int prevLimit = fPointSizeLimit;
    fPointSizeLimit = newLimit;
    return prevLimit;
}

void SkStrikeCache::forEachStrike(std::function<void(const Strike&)> visitor) const {
    SkAutoSpinlock ac(fLock);

    this->validate();

    for (Strike* strike = fHead; strike != nullptr; strike = strike->fNext) {
        visitor(*strike);
    }
}

size_t SkStrikeCache::internalPurge(size_t minBytesNeeded) {
    this->validate();

    size_t bytesNeeded = 0;
    if (fTotalMemoryUsed > fCacheSizeLimit) {
        bytesNeeded = fTotalMemoryUsed - fCacheSizeLimit;
    }
    bytesNeeded = std::max(bytesNeeded, minBytesNeeded);
    if (bytesNeeded) {
        // no small purges!
        bytesNeeded = std::max(bytesNeeded, fTotalMemoryUsed >> 2);
    }

    int countNeeded = 0;
    if (fCacheCount > fCacheCountLimit) {
        countNeeded = fCacheCount - fCacheCountLimit;
        // no small purges!
        countNeeded = std::max(countNeeded, fCacheCount >> 2);
    }

    // early exit
    if (!countNeeded && !bytesNeeded) {
        return 0;
    }

    size_t  bytesFreed = 0;
    int     countFreed = 0;

    // Start at the tail and proceed backwards deleting; the list is in LRU
    // order, with unimportant entries at the tail.
    Strike* strike = fTail;
    while (strike != nullptr && (bytesFreed < bytesNeeded || countFreed < countNeeded)) {
        Strike* prev = strike->fPrev;

        // Only delete if the strike is not pinned.
        if (strike->fPinner == nullptr || strike->fPinner->canDelete()) {
            bytesFreed += strike->fScalerCache.getMemoryUsed();
            countFreed += 1;
            this->internalDetachStrike(strike);
            strike->unref();
        }
        strike = prev;
    }

    this->validate();

#ifdef SPEW_PURGE_STATUS
    if (countFreed) {
        SkDebugf("purging %dK from font cache [%d entries]\n",
                 (int)(bytesFreed >> 10), countFreed);
    }
#endif

    return bytesFreed;
}

void SkStrikeCache::internalAttachToHead(Strike* strike) {
    SkASSERT(nullptr == strike->fPrev && nullptr == strike->fNext);
    if (fHead) {
        fHead->fPrev = strike;
        strike->fNext = fHead;
    }
    fHead = strike;

    if (fTail == nullptr) {
        fTail = strike;
    }

    fCacheCount += 1;
    fTotalMemoryUsed += strike->fScalerCache.getMemoryUsed();
}

void SkStrikeCache::internalDetachStrike(Strike* strike) {
    SkASSERT(fCacheCount > 0);
    fCacheCount -= 1;
    fTotalMemoryUsed -= strike->fScalerCache.getMemoryUsed();

    if (strike->fPrev) {
        strike->fPrev->fNext = strike->fNext;
    } else {
        fHead = strike->fNext;
    }
    if (strike->fNext) {
        strike->fNext->fPrev = strike->fPrev;
    } else {
        fTail = strike->fPrev;
    }
    strike->fPrev = strike->fNext = nullptr;
}

void SkStrikeCache::ValidateGlyphCacheDataSize() {
#ifdef SK_DEBUG
    GlobalStrikeCache()->validateGlyphCacheDataSize();
#endif
}

#ifdef SK_DEBUG
void SkStrikeCache::validateGlyphCacheDataSize() const {
    this->forEachStrike(
            [](const Strike& strike) { strike.fScalerCache.forceValidate();
    });
}
#endif

#ifdef SK_DEBUG
void SkStrikeCache::validate() const {
    size_t computedBytes = 0;
    int computedCount = 0;

    const Strike* strike = fHead;
    while (strike != nullptr) {
        computedBytes += strike->fScalerCache.getMemoryUsed();
        computedCount += 1;
        strike = strike->fNext;
    }

    // Can't use SkASSERTF because it looses thread annotations.
    if (fCacheCount != computedCount) {
        SkDebugf("fCacheCount: %d, computedCount: %d", fCacheCount, computedCount);
        SK_ABORT("fCacheCount != computedCount");
    }
    if (fTotalMemoryUsed != computedBytes) {
        SkDebugf("fTotalMemoryUsed: %d, computedBytes: %d", fTotalMemoryUsed, computedBytes);
        SK_ABORT("fTotalMemoryUsed == computedBytes");
    }
}
#endif
