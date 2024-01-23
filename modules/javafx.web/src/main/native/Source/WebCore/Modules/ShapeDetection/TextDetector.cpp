/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TextDetector.h"

#include "Chrome.h"
#include "DetectedText.h"
#include "Document.h"
#include "ImageBitmap.h"
#include "ImageBitmapOptions.h"
#include "JSDOMPromiseDeferred.h"
#include "JSDetectedText.h"
#include "Page.h"
#include "ScriptExecutionContext.h"
#include "WorkerGlobalScope.h"

namespace WebCore {

ExceptionOr<Ref<TextDetector>> TextDetector::create(ScriptExecutionContext& scriptExecutionContext)
{
    if (is<Document>(scriptExecutionContext)) {
        const auto& document = downcast<Document>(scriptExecutionContext);
        const auto* page = document.page();
        if (!page)
            return Exception { AbortError };
        auto backing = page->chrome().createTextDetector();
        if (!backing)
            return Exception { AbortError };
        return adoptRef(*new TextDetector(backing.releaseNonNull()));
    }

    if (is<WorkerGlobalScope>(scriptExecutionContext)) {
        // FIXME: https://bugs.webkit.org/show_bug.cgi?id=255380 Make the Shape Detection API work in Workers
        return Exception { AbortError };
    }

    return Exception { AbortError };
}

TextDetector::TextDetector(Ref<ShapeDetection::TextDetector>&& backing)
    : m_backing(WTFMove(backing))
{
}

TextDetector::~TextDetector() = default;

void TextDetector::detect(ScriptExecutionContext& scriptExecutionContext, ImageBitmap::Source&& source, DetectPromise&& promise)
{
    ImageBitmap::createCompletionHandler(scriptExecutionContext, WTFMove(source), { }, [backing = m_backing.copyRef(), promise = WTFMove(promise)](ExceptionOr<Ref<ImageBitmap>>&& imageBitmap) mutable {
        if (imageBitmap.hasException()) {
            promise.resolve({ });
            return;
        }

        auto imageBuffer = imageBitmap.releaseReturnValue()->takeImageBuffer();
        if (!imageBuffer) {
            promise.resolve({ });
            return;
        }

        backing->detect(imageBuffer.releaseNonNull(), [promise = WTFMove(promise)](Vector<ShapeDetection::DetectedText>&& detectedText) mutable {
            promise.resolve(detectedText.map([](const auto& detectedText) {
                return convertFromBacking(detectedText);
            }));
        });
    });
}

} // namespace WebCore
