/*
 * Copyright (C) 2022-2023 Apple Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "FilterEffect.h"
#include "FilterImageVector.h"
#include "ImageBufferAllocator.h"
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace WebCore {

class FilterEffect;
class FilterImage;

class FilterResults {
    WTF_MAKE_FAST_ALLOCATED;
public:
    WEBCORE_EXPORT FilterResults(std::unique_ptr<ImageBufferAllocator>&& = nullptr);

    ImageBufferAllocator& allocator() const { return *m_allocator; }

    FilterImage* effectResult(FilterEffect&) const;
    void setEffectResult(FilterEffect&, const FilterImageVector& inputs, Ref<FilterImage>&& result);
    void clearEffectResult(FilterEffect&);

private:
    size_t memoryCost() const;
    bool canCacheResult(const FilterImage&) const;

    HashMap<Ref<FilterEffect>, Ref<FilterImage>> m_results;

    // The value is a list of FilterEffects, whose FilterImages depend on the key FilterImage.
    using FilterEffectSet = HashSet<Ref<FilterEffect>>;
    HashMap<Ref<FilterImage>, FilterEffectSet> m_resultReferences;

    std::unique_ptr<ImageBufferAllocator> m_allocator;
};

using FilterResultsCreator = Function<std::unique_ptr<FilterResults>()>;

} // namespace WebCore
