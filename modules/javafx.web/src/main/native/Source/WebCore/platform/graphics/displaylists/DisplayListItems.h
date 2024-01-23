/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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

#include "AlphaPremultiplication.h"
#include "DisplayListItemBufferIdentifier.h"
#include "DisplayListItemType.h"
#include "DisplayListResourceHeap.h"
#include "FloatRoundedRect.h"
#include "Font.h"
#include "GlyphBuffer.h"
#include "Gradient.h"
#include "GraphicsContext.h"
#include "Image.h"
#include "MediaPlayerIdentifier.h"
#include "Pattern.h"
#include "PixelBuffer.h"
#include "PositionedGlyphs.h"
#include "RenderingResourceIdentifier.h"
#include "SharedBuffer.h"
#include "SystemImage.h"
#include <variant>
#include <wtf/ArgumentCoder.h>
#include <wtf/EnumTraits.h>
#include <wtf/TypeCasts.h>

namespace WTF {
class TextStream;
}

namespace WebCore {

class MediaPlayer;
struct ImagePaintingOptions;

namespace DisplayList {

enum class AsTextFlag : uint8_t;
struct ItemHandle;

/* isInlineItem indicates whether the object needs to be passed through IPC::Encoder in order to serialize,
 * or whether we can just use placement new and be done.
 * It needs to match (1) RemoteImageBufferProxy::encodeItem(), (2) RemoteRenderingBackend::decodeItem(),
 * and (3) isInlineItem() in DisplayListItemType.cpp.
 *
 * isDrawingItem indicates if this command can affect dirty rects.
 * We can do things like skip drawing items when replaying them if their extents don't intersect with the current clip.
 * It needs to match isDrawingItem() in DisplayListItemType.cpp. */

class Save {
public:
    static constexpr ItemType itemType = ItemType::Save;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

class Restore {
public:
    static constexpr ItemType itemType = ItemType::Restore;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

class Translate {
public:
    static constexpr ItemType itemType = ItemType::Translate;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    Translate(float x, float y)
        : m_x(x)
        , m_y(y)
    {
    }

    float x() const { return m_x; }
    float y() const { return m_y; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_x { 0 };
    float m_y { 0 };
};

class Rotate {
public:
    static constexpr ItemType itemType = ItemType::Rotate;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    Rotate(float angle)
        : m_angle(angle)
    {
    }

    float angle() const { return m_angle; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_angle { 0 }; // In radians.
};

class Scale {
public:
    static constexpr ItemType itemType = ItemType::Scale;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    Scale(const FloatSize& size)
        : m_size(size)
    {
    }

    const FloatSize& amount() const { return m_size; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatSize m_size;
};

class SetCTM {
public:
    static constexpr ItemType itemType = ItemType::SetCTM;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetCTM(const AffineTransform& transform)
        : m_transform(transform)
    {
    }

    const AffineTransform& transform() const { return m_transform; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    AffineTransform m_transform;
};

class ConcatenateCTM {
public:
    static constexpr ItemType itemType = ItemType::ConcatenateCTM;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ConcatenateCTM(const AffineTransform& transform)
        : m_transform(transform)
    {
    }

    const AffineTransform& transform() const { return m_transform; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    AffineTransform m_transform;
};

class SetInlineFillColor {
public:
    static constexpr ItemType itemType = ItemType::SetInlineFillColor;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetInlineFillColor() = default;
    SetInlineFillColor(SRGBA<uint8_t> colorData)
        : m_colorData(colorData)
    {
    }
    SetInlineFillColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
        : SetInlineFillColor(SRGBA<uint8_t> { red, green, blue, alpha }) { }

    Color color() const { return { m_colorData }; }
    const SRGBA<uint8_t>& colorData() const { return m_colorData; }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    SRGBA<uint8_t> m_colorData;
};

class SetInlineStrokeColor {
public:
    static constexpr ItemType itemType = ItemType::SetInlineStrokeColor;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetInlineStrokeColor() = default;
    SetInlineStrokeColor(SRGBA<uint8_t> colorData)
        : m_colorData(colorData)
    {
    }
    SetInlineStrokeColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
        : SetInlineStrokeColor(SRGBA<uint8_t> { red, green, blue, alpha }) { }

    Color color() const { return { m_colorData }; }
    const SRGBA<uint8_t>& colorData() const { return m_colorData; }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    SRGBA<uint8_t> m_colorData;
};

class SetStrokeThickness {
public:
    static constexpr ItemType itemType = ItemType::SetStrokeThickness;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetStrokeThickness(float thickness)
        : m_thickness(thickness)
    {
    }

    float thickness() const { return m_thickness; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_thickness { 0 };
};

class SetState {
public:
    static constexpr ItemType itemType = ItemType::SetState;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT SetState(const GraphicsContextState&);

    GraphicsContextState& state() { return m_state; }
    const GraphicsContextState& state() const { return m_state; }

    WEBCORE_EXPORT void apply(GraphicsContext&);

private:
    GraphicsContextState m_state;
};

class SetLineCap {
public:
    static constexpr ItemType itemType = ItemType::SetLineCap;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetLineCap(LineCap lineCap)
        : m_lineCap(lineCap)
    {
    }

    LineCap lineCap() const { return m_lineCap; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    LineCap m_lineCap;
};

class SetLineDash {
public:
    static constexpr ItemType itemType = ItemType::SetLineDash;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = false;

    SetLineDash(const DashArray& dashArray, float dashOffset)
        : m_dashArray(dashArray)
        , m_dashOffset(dashOffset)
    {
    }

    const DashArray& dashArray() const { return m_dashArray; }
    float dashOffset() const { return m_dashOffset; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    DashArray m_dashArray;
    float m_dashOffset;
};

class SetLineJoin {
public:
    static constexpr ItemType itemType = ItemType::SetLineJoin;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetLineJoin(LineJoin lineJoin)
        : m_lineJoin(lineJoin)
    {
    }

    LineJoin lineJoin() const { return m_lineJoin; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    LineJoin m_lineJoin;
};

class SetMiterLimit {
public:
    static constexpr ItemType itemType = ItemType::SetMiterLimit;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    SetMiterLimit(float miterLimit)
        : m_miterLimit(miterLimit)
    {
    }

    float miterLimit() const { return m_miterLimit; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_miterLimit;
};

class ClearShadow {
public:
    static constexpr ItemType itemType = ItemType::ClearShadow;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

// FIXME: treat as drawing item?
class Clip {
public:
    static constexpr ItemType itemType = ItemType::Clip;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    Clip(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class ClipRoundedRect {
public:
    static constexpr ItemType itemType = ItemType::ClipRoundedRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ClipRoundedRect(const FloatRoundedRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRoundedRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRoundedRect m_rect;
};

class ClipOut {
public:
    static constexpr ItemType itemType = ItemType::ClipOut;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ClipOut(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class ClipOutRoundedRect {
public:
    static constexpr ItemType itemType = ItemType::ClipOutRoundedRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ClipOutRoundedRect(const FloatRoundedRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRoundedRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRoundedRect m_rect;
};

class ClipToImageBuffer {
public:
    static constexpr ItemType itemType = ItemType::ClipToImageBuffer;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ClipToImageBuffer(RenderingResourceIdentifier imageBufferIdentifier, const FloatRect& destinationRect)
        : m_imageBufferIdentifier(imageBufferIdentifier)
        , m_destinationRect(destinationRect)
    {
    }

    RenderingResourceIdentifier imageBufferIdentifier() const { return m_imageBufferIdentifier; }
    FloatRect destinationRect() const { return m_destinationRect; }
    bool isValid() const { return m_imageBufferIdentifier.isValid(); }

    WEBCORE_EXPORT void apply(GraphicsContext&, ImageBuffer&) const;

private:
    RenderingResourceIdentifier m_imageBufferIdentifier;
    FloatRect m_destinationRect;
};

class ClipOutToPath {
public:
    static constexpr ItemType itemType = ItemType::ClipOutToPath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = false;

    ClipOutToPath(Path&& path)
        : m_path(WTFMove(path))
    {
    }

    ClipOutToPath(const Path& path)
        : m_path(path)
    {
    }

    const Path& path() const { return m_path; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
};

class ClipPath {
public:
    static constexpr ItemType itemType = ItemType::ClipPath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = false;

    ClipPath(Path&& path, WindRule windRule)
        : m_path(WTFMove(path))
        , m_windRule(windRule)
    {
    }

    ClipPath(const Path& path, WindRule windRule)
        : m_path(path)
        , m_windRule(windRule)
    {
    }

    const Path& path() const { return m_path; }
    WindRule windRule() const { return m_windRule; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
    WindRule m_windRule;
};

class ResetClip {
public:
    static constexpr ItemType itemType = ItemType::ResetClip;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ResetClip()
    {
    }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

class DrawFilteredImageBuffer {
public:
    static constexpr ItemType itemType = ItemType::DrawFilteredImageBuffer;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT DrawFilteredImageBuffer(std::optional<RenderingResourceIdentifier> sourceImageIdentifier, const FloatRect& sourceImageRect, Filter&);

    std::optional<RenderingResourceIdentifier> sourceImageIdentifier() const { return m_sourceImageIdentifier; }
    FloatRect sourceImageRect() const { return m_sourceImageRect; }

    WEBCORE_EXPORT void apply(GraphicsContext&, ImageBuffer* sourceImage, FilterResults&);

private:
    std::optional<RenderingResourceIdentifier> m_sourceImageIdentifier;
    FloatRect m_sourceImageRect;
    Ref<Filter> m_filter;
};

class DrawGlyphs {
public:
    static constexpr ItemType itemType = ItemType::DrawGlyphs;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    RenderingResourceIdentifier fontIdentifier() const { return m_fontIdentifier; }
    PositionedGlyphs positionedGlyphs() const { return m_positionedGlyphs; }
    const FloatPoint& localAnchor() const { return m_positionedGlyphs.localAnchor; }
    FloatPoint anchorPoint() const { return m_positionedGlyphs.localAnchor; }
    FontSmoothingMode fontSmoothingMode() const { return m_positionedGlyphs.smoothingMode; }
    const Vector<GlyphBufferGlyph>& glyphs() const { return m_positionedGlyphs.glyphs; }

    WEBCORE_EXPORT DrawGlyphs(const Font&, const GlyphBufferGlyph*, const GlyphBufferAdvance*, unsigned count, const FloatPoint& localAnchor, FontSmoothingMode);
    WEBCORE_EXPORT DrawGlyphs(RenderingResourceIdentifier, PositionedGlyphs&&);

    WEBCORE_EXPORT void apply(GraphicsContext&, const Font&) const;

private:
    RenderingResourceIdentifier m_fontIdentifier;
    PositionedGlyphs m_positionedGlyphs;
};

class DrawDecomposedGlyphs {
public:
    static constexpr ItemType itemType = ItemType::DrawDecomposedGlyphs;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawDecomposedGlyphs(RenderingResourceIdentifier fontIdentifier, RenderingResourceIdentifier decomposedGlyphsIdentifier)
        : m_fontIdentifier(fontIdentifier)
        , m_decomposedGlyphsIdentifier(decomposedGlyphsIdentifier)
    {
    }

    RenderingResourceIdentifier fontIdentifier() const { return m_fontIdentifier; }
    RenderingResourceIdentifier decomposedGlyphsIdentifier() const { return m_decomposedGlyphsIdentifier; }

    WEBCORE_EXPORT void apply(GraphicsContext&, const Font&, const DecomposedGlyphs&) const;

private:
    RenderingResourceIdentifier m_fontIdentifier;
    RenderingResourceIdentifier m_decomposedGlyphsIdentifier;
};

class DrawImageBuffer {
public:
    static constexpr ItemType itemType = ItemType::DrawImageBuffer;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawImageBuffer(RenderingResourceIdentifier imageBufferIdentifier, const FloatRect& destRect, const FloatRect& srcRect, const ImagePaintingOptions& options)
        : m_imageBufferIdentifier(imageBufferIdentifier)
        , m_destinationRect(destRect)
        , m_srcRect(srcRect)
        , m_options(options)
    {
    }

    RenderingResourceIdentifier imageBufferIdentifier() const { return m_imageBufferIdentifier; }
    FloatRect source() const { return m_srcRect; }
    FloatRect destinationRect() const { return m_destinationRect; }
    ImagePaintingOptions options() const { return m_options; }
    // FIXME: We might want to validate ImagePaintingOptions.
    bool isValid() const { return m_imageBufferIdentifier.isValid(); }

    WEBCORE_EXPORT void apply(GraphicsContext&, ImageBuffer&) const;

private:
    RenderingResourceIdentifier m_imageBufferIdentifier;
    FloatRect m_destinationRect;
    FloatRect m_srcRect;
    ImagePaintingOptions m_options;
};

class DrawNativeImage {
public:
    static constexpr ItemType itemType = ItemType::DrawNativeImage;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawNativeImage(RenderingResourceIdentifier imageIdentifier, const FloatSize& imageSize, const FloatRect& destRect, const FloatRect& srcRect, const ImagePaintingOptions& options)
        : m_imageIdentifier(imageIdentifier)
        , m_imageSize(imageSize)
        , m_destinationRect(destRect)
        , m_srcRect(srcRect)
        , m_options(options)
    {
    }

    RenderingResourceIdentifier imageIdentifier() const { return m_imageIdentifier; }
    const FloatRect& source() const { return m_srcRect; }
    const FloatRect& destinationRect() const { return m_destinationRect; }
    // FIXME: We might want to validate ImagePaintingOptions.
    bool isValid() const { return m_imageIdentifier.isValid(); }

    WEBCORE_EXPORT void apply(GraphicsContext&, NativeImage&) const;

private:
    RenderingResourceIdentifier m_imageIdentifier;
    FloatSize m_imageSize;
    FloatRect m_destinationRect;
    FloatRect m_srcRect;
    ImagePaintingOptions m_options;
};

class DrawSystemImage {
public:
    static constexpr ItemType itemType = ItemType::DrawSystemImage;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    DrawSystemImage(SystemImage& systemImage, const FloatRect& destinationRect)
        : m_systemImage(systemImage)
        , m_destinationRect(destinationRect)
    {
    }

    const Ref<SystemImage>& systemImage() const { return m_systemImage; }
    const FloatRect& destinationRect() const { return m_destinationRect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Ref<SystemImage> m_systemImage;
    FloatRect m_destinationRect;
};

class DrawPattern {
public:
    static constexpr ItemType itemType = ItemType::DrawPattern;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT DrawPattern(RenderingResourceIdentifier, const FloatRect& destRect, const FloatRect& tileRect, const AffineTransform&, const FloatPoint& phase, const FloatSize& spacing, const ImagePaintingOptions& = { });

    RenderingResourceIdentifier imageIdentifier() const { return m_imageIdentifier; }
    FloatRect destRect() const { return m_destination; }
    FloatRect tileRect() const { return m_tileRect; }
    const AffineTransform& patternTransform() const { return m_patternTransform; }
    FloatPoint phase() const { return m_phase; }
    FloatSize spacing() const { return m_spacing; }
    // FIXME: We might want to validate ImagePaintingOptions.
    bool isValid() const { return m_imageIdentifier.isValid(); }

    WEBCORE_EXPORT void apply(GraphicsContext&, SourceImage&) const;

private:
    RenderingResourceIdentifier m_imageIdentifier;
    FloatRect m_destination;
    FloatRect m_tileRect;
    AffineTransform m_patternTransform;
    FloatPoint m_phase;
    FloatSize m_spacing;
    ImagePaintingOptions m_options;
};

class BeginTransparencyLayer {
public:
    static constexpr ItemType itemType = ItemType::BeginTransparencyLayer;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true; // Is drawing item because the size of the transparency layer is implicitly the clip bounds.

    BeginTransparencyLayer(float opacity)
        : m_opacity(opacity)
    {
    }

    float opacity() const { return m_opacity; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_opacity;
};

class EndTransparencyLayer {
public:
    static constexpr ItemType itemType = ItemType::EndTransparencyLayer;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

};
class DrawRect {
public:
    static constexpr ItemType itemType = ItemType::DrawRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawRect(FloatRect rect, float borderThickness)
        : m_rect(rect)
        , m_borderThickness(borderThickness)
    {
    }

    FloatRect rect() const { return m_rect; }
    float borderThickness() const { return m_borderThickness; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    float m_borderThickness;
};

class DrawLine {
public:
    static constexpr ItemType itemType = ItemType::DrawLine;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawLine(FloatPoint point1, FloatPoint point2)
        : m_point1(point1)
        , m_point2(point2)
    {
    }

    FloatPoint point1() const { return m_point1; }
    FloatPoint point2() const { return m_point2; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatPoint m_point1;
    FloatPoint m_point2;
};

class DrawLinesForText {
public:
    static constexpr ItemType itemType = ItemType::DrawLinesForText;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT DrawLinesForText(const FloatPoint& blockLocation, const FloatSize& localAnchor, const DashArray& widths, float thickness, bool printing, bool doubleLines, StrokeStyle);

    void setBlockLocation(const FloatPoint& blockLocation) { m_blockLocation = blockLocation; }
    const FloatPoint& blockLocation() const { return m_blockLocation; }
    const FloatSize& localAnchor() const { return m_localAnchor; }
    FloatPoint point() const { return m_blockLocation + m_localAnchor; }
    float thickness() const { return m_thickness; }
    const DashArray& widths() const { return m_widths; }
    bool isPrinting() const { return m_printing; }
    bool doubleLines() const { return m_doubleLines; }
    StrokeStyle style() const { return m_style; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatPoint m_blockLocation;
    FloatSize m_localAnchor;
    DashArray m_widths;
    float m_thickness;
    bool m_printing;
    bool m_doubleLines;
    StrokeStyle m_style;
};

class DrawDotsForDocumentMarker {
public:
    static constexpr ItemType itemType = ItemType::DrawDotsForDocumentMarker;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    DrawDotsForDocumentMarker(const FloatRect& rect, const DocumentMarkerLineStyle& style)
        : m_rect(rect)
        , m_style(style)
    {
    }

    FloatRect rect() const { return m_rect; }
    DocumentMarkerLineStyle style() const { return m_style; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    DocumentMarkerLineStyle m_style;
};

class DrawEllipse {
public:
    static constexpr ItemType itemType = ItemType::DrawEllipse;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    DrawEllipse(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    FloatRect rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class DrawPath {
public:
    static constexpr ItemType itemType = ItemType::DrawPath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    DrawPath(const Path& path)
        : m_path(path)
    {
    }

    DrawPath(Path&& path)
        : m_path(WTFMove(path))
    {
    }

    const Path& path() const { return m_path; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
};

class DrawFocusRingPath {
public:
    static constexpr ItemType itemType = ItemType::DrawFocusRingPath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    DrawFocusRingPath(const Path& path, float outlineWidth, const Color& color)
        : m_path(path)
        , m_outlineWidth(outlineWidth)
        , m_color(color)
    {
    }

    DrawFocusRingPath(Path&& path, float outlineWidth, const Color& color)
        : m_path(WTFMove(path))
        , m_outlineWidth(outlineWidth)
        , m_color(color)
    {
    }

    const Path& path() const { return m_path; }
    float outlineWidth() const { return m_outlineWidth; }
    const Color& color() const { return m_color; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
    float m_outlineWidth;
    Color m_color;
};

class DrawFocusRingRects {
public:
    static constexpr ItemType itemType = ItemType::DrawFocusRingRects;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    DrawFocusRingRects(const Vector<FloatRect>& rects, float outlineOffset, float outlineWidth, const Color& color)
        : m_rects(rects)
        , m_outlineOffset(outlineOffset)
        , m_outlineWidth(outlineWidth)
        , m_color(color)
    {
    }

    DrawFocusRingRects(Vector<FloatRect>&& rects, float outlineOffset, float outlineWidth, Color color)
        : m_rects(WTFMove(rects))
        , m_outlineOffset(outlineOffset)
        , m_outlineWidth(outlineWidth)
        , m_color(color)
    {
    }

    const Vector<FloatRect>& rects() const { return m_rects; }
    float outlineOffset() const { return m_outlineOffset; }
    float outlineWidth() const { return m_outlineWidth; }
    const Color& color() const { return m_color; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Vector<FloatRect> m_rects;
    float m_outlineOffset;
    float m_outlineWidth;
    Color m_color;
};

class FillRect {
public:
    static constexpr ItemType itemType = ItemType::FillRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillRect(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class FillRectWithColor {
public:
    static constexpr ItemType itemType = ItemType::FillRectWithColor;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    FillRectWithColor(const FloatRect& rect, const Color& color)
        : m_rect(rect)
        , m_color(color)
    {
    }

    FloatRect rect() const { return m_rect; }
    const Color& color() const { return m_color; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    Color m_color;
};

class FillRectWithGradient {
public:
    static constexpr ItemType itemType = ItemType::FillRectWithGradient;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT FillRectWithGradient(const FloatRect&, Gradient&);
    WEBCORE_EXPORT FillRectWithGradient(FloatRect&&, Ref<Gradient>&&);

    const FloatRect& rect() const { return m_rect; }
    const Ref<Gradient>& gradient() const { return m_gradient; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    mutable Ref<Gradient> m_gradient; // FIXME: Make this not mutable
};

class FillCompositedRect {
public:
    static constexpr ItemType itemType = ItemType::FillCompositedRect;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    FillCompositedRect(const FloatRect& rect, const Color& color, CompositeOperator op, BlendMode blendMode)
        : m_rect(rect)
        , m_color(color)
        , m_op(op)
        , m_blendMode(blendMode)
    {
    }

    FloatRect rect() const { return m_rect; }
    const Color& color() const { return m_color; }
    CompositeOperator compositeOperator() const { return m_op; }
    BlendMode blendMode() const { return m_blendMode; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    Color m_color;
    CompositeOperator m_op;
    BlendMode m_blendMode;
};

class FillRoundedRect {
public:
    static constexpr ItemType itemType = ItemType::FillRoundedRect;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    FillRoundedRect(const FloatRoundedRect& rect, const Color& color, BlendMode blendMode)
        : m_rect(rect)
        , m_color(color)
        , m_blendMode(blendMode)
    {
    }

    const FloatRoundedRect& roundedRect() const { return m_rect; }
    const Color& color() const { return m_color; }
    BlendMode blendMode() const { return m_blendMode; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRoundedRect m_rect;
    Color m_color;
    BlendMode m_blendMode;
};

class FillRectWithRoundedHole {
public:
    static constexpr ItemType itemType = ItemType::FillRectWithRoundedHole;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    FillRectWithRoundedHole(const FloatRect& rect, const FloatRoundedRect& roundedHoleRect, const Color& color)
        : m_rect(rect)
        , m_roundedHoleRect(roundedHoleRect)
        , m_color(color)
    {
    }

    const FloatRect& rect() const { return m_rect; }
    const FloatRoundedRect& roundedHoleRect() const { return m_roundedHoleRect; }
    const Color& color() const { return m_color; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    FloatRoundedRect m_roundedHoleRect;
    Color m_color;
};

#if ENABLE(INLINE_PATH_DATA)

class FillLine {
public:
    static constexpr ItemType itemType = ItemType::FillLine;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillLine(const PathDataLine& line)
        : m_line(line)
    {
    }

    Path path() const { return Path({ PathSegment(m_line) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathDataLine m_line;
};

class FillArc {
public:
    static constexpr ItemType itemType = ItemType::FillArc;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillArc(const PathArc& arc)
        : m_arc(arc)
    {
    }

    Path path() const { return Path({ PathSegment(m_arc) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathArc m_arc;
};

class FillQuadCurve {
public:
    static constexpr ItemType itemType = ItemType::FillQuadCurve;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillQuadCurve(const PathDataQuadCurve& quadCurve)
        : m_quadCurve(quadCurve)
    {
    }

    Path path() const { return Path({ PathSegment(m_quadCurve) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathDataQuadCurve m_quadCurve;
};

class FillBezierCurve {
public:
    static constexpr ItemType itemType = ItemType::FillBezierCurve;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillBezierCurve(const PathDataBezierCurve& bezierCurve)
        : m_bezierCurve(bezierCurve)
    {
    }

    Path path() const { return Path({ PathSegment(m_bezierCurve) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathDataBezierCurve m_bezierCurve;
};

#endif // ENABLE(INLINE_PATH_DATA)

class FillPathSegment {
public:
    static constexpr ItemType itemType = ItemType::FillPathSegment;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillPathSegment(const PathSegment& segment)
        : m_segment(segment)
    {
    }

    Path path() const { return Path({ m_segment }); }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    PathSegment m_segment;
};

class FillPath {
public:
    static constexpr ItemType itemType = ItemType::FillPath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    FillPath(Path&& path)
        : m_path(WTFMove(path))
    {
    }

    FillPath(const Path& path)
        : m_path(path)
    {
    }

    const Path& path() const { return m_path; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
};

class FillEllipse {
public:
    static constexpr ItemType itemType = ItemType::FillEllipse;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    FillEllipse(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    FloatRect rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

#if ENABLE(VIDEO)
class PaintFrameForMedia {
public:
    static constexpr ItemType itemType = ItemType::PaintFrameForMedia;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    PaintFrameForMedia(MediaPlayer&, const FloatRect& destination);

    const FloatRect& destination() const { return m_destination; }
    MediaPlayerIdentifier identifier() const { return m_identifier; }

    bool isValid() const { return m_identifier.isValid(); }

private:
    MediaPlayerIdentifier m_identifier;
    FloatRect m_destination;
};
#endif

class StrokeRect {
public:
    static constexpr ItemType itemType = ItemType::StrokeRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokeRect(const FloatRect& rect, float lineWidth)
        : m_rect(rect)
        , m_lineWidth(lineWidth)
    {
    }

    FloatRect rect() const { return m_rect; }
    float lineWidth() const { return m_lineWidth; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
    float m_lineWidth;
};

class StrokeLine {
public:
    static constexpr ItemType itemType = ItemType::StrokeLine;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

#if ENABLE(INLINE_PATH_DATA)
    StrokeLine(const PathDataLine& line)
        : m_start(line.start)
        , m_end(line.end)
    {
    }
#endif
    StrokeLine(const FloatPoint& start, const FloatPoint& end)
        : m_start(start)
        , m_end(end)
    {
    }

    FloatPoint start() const { return m_start; }
    FloatPoint end() const { return m_end; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatPoint m_start;
    FloatPoint m_end;
};

#if ENABLE(INLINE_PATH_DATA)

class StrokeArc {
public:
    static constexpr ItemType itemType = ItemType::StrokeArc;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokeArc(const PathArc& arc)
        : m_arc(arc)
    {
    }

    Path path() const { return Path({ PathSegment(m_arc) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathArc m_arc;
};

class StrokeQuadCurve {
public:
    static constexpr ItemType itemType = ItemType::StrokeQuadCurve;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokeQuadCurve(const PathDataQuadCurve& quadCurve)
        : m_quadCurve(quadCurve)
    {
    }

    Path path() const { return Path({ PathSegment(m_quadCurve) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;
private:
    PathDataQuadCurve m_quadCurve;
};

class StrokeBezierCurve {
public:
    static constexpr ItemType itemType = ItemType::StrokeBezierCurve;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokeBezierCurve(const PathDataBezierCurve& bezierCurve)
        : m_bezierCurve(bezierCurve)
    {
    }

    Path path() const { return Path({ PathSegment(m_bezierCurve) }); }
    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    PathDataBezierCurve m_bezierCurve;
};

#endif // ENABLE(INLINE_PATH_DATA)

class StrokePathSegment {
public:
    static constexpr ItemType itemType = ItemType::StrokePathSegment;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokePathSegment(const PathSegment& segment)
        : m_segment(segment)
    {
    }

    Path path() const { return Path({ m_segment }); }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    PathSegment m_segment;
};

class StrokePath {
public:
    static constexpr ItemType itemType = ItemType::StrokePath;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    StrokePath(Path&& path)
        : m_path(WTFMove(path))
    {
    }

    StrokePath(const Path& path)
        : m_path(path)
    {
    }

    const Path& path() const { return m_path; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    Path m_path;
};

class StrokeEllipse {
public:
    static constexpr ItemType itemType = ItemType::StrokeEllipse;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    StrokeEllipse(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class ClearRect {
public:
    static constexpr ItemType itemType = ItemType::ClearRect;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = true;

    ClearRect(const FloatRect& rect)
        : m_rect(rect)
    {
    }

    const FloatRect& rect() const { return m_rect; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    FloatRect m_rect;
};

class DrawControlPart {
public:
    static constexpr ItemType itemType = ItemType::DrawControlPart;
    static constexpr bool isInlineItem = false;
    static constexpr bool isDrawingItem = true;

    WEBCORE_EXPORT DrawControlPart(ControlPart&, const FloatRoundedRect& borderRect, float deviceScaleFactor, const ControlStyle&);

    StyleAppearance type() const { return m_part->type(); }
    FloatRoundedRect borderRect() const { return m_borderRect; }
    float deviceScaleFactor() const { return m_deviceScaleFactor; }
    const ControlStyle& style() const { return m_style; }

    WEBCORE_EXPORT void apply(GraphicsContext&);

private:
    Ref<ControlPart> m_part;
    FloatRoundedRect m_borderRect;
    float m_deviceScaleFactor;
    ControlStyle m_style;
};

#if USE(CG)

class ApplyStrokePattern {
public:
    static constexpr ItemType itemType = ItemType::ApplyStrokePattern;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

class ApplyFillPattern {
public:
    static constexpr ItemType itemType = ItemType::ApplyFillPattern;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    WEBCORE_EXPORT void apply(GraphicsContext&) const;
};

#endif

class ApplyDeviceScaleFactor {
public:
    static constexpr ItemType itemType = ItemType::ApplyDeviceScaleFactor;
    static constexpr bool isInlineItem = true;
    static constexpr bool isDrawingItem = false;

    ApplyDeviceScaleFactor(float scaleFactor)
        : m_scaleFactor(scaleFactor)
    {
    }

    float scaleFactor() const { return m_scaleFactor; }

    WEBCORE_EXPORT void apply(GraphicsContext&) const;

private:
    float m_scaleFactor { 1 };
};

using DisplayListItem = std::variant
    < ApplyDeviceScaleFactor
    , BeginTransparencyLayer
    , ClearRect
    , ClearShadow
    , Clip
    , ClipRoundedRect
    , ClipOut
    , ClipOutRoundedRect
    , ClipOutToPath
    , ClipPath
    , ClipToImageBuffer
    , ConcatenateCTM
    , DrawControlPart
    , DrawDotsForDocumentMarker
    , DrawEllipse
    , DrawFilteredImageBuffer
    , DrawFocusRingPath
    , DrawFocusRingRects
    , DrawGlyphs
    , DrawDecomposedGlyphs
    , DrawImageBuffer
    , DrawLine
    , DrawLinesForText
    , DrawNativeImage
    , DrawPath
    , DrawPattern
    , DrawRect
    , DrawSystemImage
    , EndTransparencyLayer
    , FillCompositedRect
    , FillEllipse
    , FillPathSegment
    , FillPath
    , FillRect
    , FillRectWithColor
    , FillRectWithGradient
    , FillRectWithRoundedHole
    , FillRoundedRect
    , ResetClip
    , Restore
    , Rotate
    , Save
    , Scale
    , SetCTM
    , SetInlineFillColor
    , SetInlineStrokeColor
    , SetLineCap
    , SetLineDash
    , SetLineJoin
    , SetMiterLimit
    , SetState
    , SetStrokeThickness
    , StrokeEllipse
    , StrokeLine
    , StrokePathSegment
    , StrokePath
    , StrokeRect
    , Translate

#if ENABLE(INLINE_PATH_DATA)
    , FillLine
    , FillArc
    , FillQuadCurve
    , FillBezierCurve
    , StrokeArc
    , StrokeQuadCurve
    , StrokeBezierCurve
#endif

#if ENABLE(VIDEO)
    , PaintFrameForMedia
#endif

#if USE(CG)
    , ApplyFillPattern
    , ApplyStrokePattern
#endif
>;

size_t paddedSizeOfTypeAndItemInBytes(const DisplayListItem&);
ItemType displayListItemType(const DisplayListItem&);

WEBCORE_EXPORT void dumpItem(TextStream&, const Translate&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const Rotate&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const Scale&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetCTM&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ConcatenateCTM&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetInlineFillColor&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetInlineStrokeColor&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetStrokeThickness&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetState&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetLineCap&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetLineDash&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetLineJoin&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const SetMiterLimit&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const Clip&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipRoundedRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipOut&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipOutRoundedRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipToImageBuffer&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipOutToPath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClipPath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ResetClip&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawControlPart&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawFilteredImageBuffer&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawGlyphs&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawDecomposedGlyphs&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawImageBuffer&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawNativeImage&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawSystemImage&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawPattern&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawLine&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawLinesForText&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawDotsForDocumentMarker&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawEllipse&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawPath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawFocusRingPath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const DrawFocusRingRects&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillRectWithColor&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillRectWithGradient&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillCompositedRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillRoundedRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillRectWithRoundedHole&, OptionSet<AsTextFlag>);
#if ENABLE(INLINE_PATH_DATA)
WEBCORE_EXPORT void dumpItem(TextStream&, const FillLine&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillArc&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillQuadCurve&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillBezierCurve&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeArc&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeQuadCurve&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeBezierCurve&, OptionSet<AsTextFlag>);
#endif // ENABLE(INLINE_PATH_DATA)
WEBCORE_EXPORT void dumpItem(TextStream&, const FillPathSegment&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillPath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const FillEllipse&, OptionSet<AsTextFlag>);
#if ENABLE(VIDEO)
WEBCORE_EXPORT void dumpItem(TextStream&, const PaintFrameForMedia&, OptionSet<AsTextFlag>);
#endif // ENABLE(VIDEO)
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokePathSegment&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokePath&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeEllipse&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const StrokeLine&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ClearRect&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const BeginTransparencyLayer&, OptionSet<AsTextFlag>);
WEBCORE_EXPORT void dumpItem(TextStream&, const ApplyDeviceScaleFactor&, OptionSet<AsTextFlag>);

template <typename T>
TextStream& operator<<(TextStream& ts, const T& item)
{
    dumpItem(ts, item, { AsTextFlag::IncludePlatformOperations, AsTextFlag::IncludeResourceIdentifiers });
    return ts;
}

void dumpItemHandle(TextStream&, const ItemHandle&, OptionSet<AsTextFlag>);

inline TextStream& operator<<(TextStream& ts, const ItemHandle& itemHandle)
{
    dumpItemHandle(ts, itemHandle, { AsTextFlag::IncludePlatformOperations, AsTextFlag::IncludeResourceIdentifiers });
    return ts;
}

TextStream& operator<<(TextStream&, ItemType);

} // namespace DisplayList
} // namespace WebCore

namespace WTF {

template<> struct EnumTraits<WebCore::DisplayList::ItemType> {
    using values = EnumValues<
    WebCore::DisplayList::ItemType,
    WebCore::DisplayList::ItemType::Save,
    WebCore::DisplayList::ItemType::Restore,
    WebCore::DisplayList::ItemType::Translate,
    WebCore::DisplayList::ItemType::Rotate,
    WebCore::DisplayList::ItemType::Scale,
    WebCore::DisplayList::ItemType::SetCTM,
    WebCore::DisplayList::ItemType::ConcatenateCTM,
    WebCore::DisplayList::ItemType::SetInlineFillColor,
    WebCore::DisplayList::ItemType::SetInlineStrokeColor,
    WebCore::DisplayList::ItemType::SetStrokeThickness,
    WebCore::DisplayList::ItemType::SetState,
    WebCore::DisplayList::ItemType::SetLineCap,
    WebCore::DisplayList::ItemType::SetLineDash,
    WebCore::DisplayList::ItemType::SetLineJoin,
    WebCore::DisplayList::ItemType::SetMiterLimit,
    WebCore::DisplayList::ItemType::ClearShadow,
    WebCore::DisplayList::ItemType::Clip,
    WebCore::DisplayList::ItemType::ClipRoundedRect,
    WebCore::DisplayList::ItemType::ClipOut,
    WebCore::DisplayList::ItemType::ClipOutRoundedRect,
    WebCore::DisplayList::ItemType::ClipToImageBuffer,
    WebCore::DisplayList::ItemType::ClipOutToPath,
    WebCore::DisplayList::ItemType::ClipPath,
    WebCore::DisplayList::ItemType::ResetClip,
    WebCore::DisplayList::ItemType::DrawGlyphs,
    WebCore::DisplayList::ItemType::DrawDecomposedGlyphs,
    WebCore::DisplayList::ItemType::DrawImageBuffer,
    WebCore::DisplayList::ItemType::DrawNativeImage,
    WebCore::DisplayList::ItemType::DrawSystemImage,
    WebCore::DisplayList::ItemType::DrawPattern,
    WebCore::DisplayList::ItemType::DrawRect,
    WebCore::DisplayList::ItemType::DrawLine,
    WebCore::DisplayList::ItemType::DrawLinesForText,
    WebCore::DisplayList::ItemType::DrawDotsForDocumentMarker,
    WebCore::DisplayList::ItemType::DrawEllipse,
    WebCore::DisplayList::ItemType::DrawPath,
    WebCore::DisplayList::ItemType::DrawFocusRingPath,
    WebCore::DisplayList::ItemType::DrawFocusRingRects,
    WebCore::DisplayList::ItemType::FillRect,
    WebCore::DisplayList::ItemType::FillRectWithColor,
    WebCore::DisplayList::ItemType::FillRectWithGradient,
    WebCore::DisplayList::ItemType::FillCompositedRect,
    WebCore::DisplayList::ItemType::FillRoundedRect,
    WebCore::DisplayList::ItemType::FillRectWithRoundedHole,
#if ENABLE(INLINE_PATH_DATA)
    WebCore::DisplayList::ItemType::FillLine,
    WebCore::DisplayList::ItemType::FillArc,
    WebCore::DisplayList::ItemType::FillQuadCurve,
    WebCore::DisplayList::ItemType::FillBezierCurve,
#endif
    WebCore::DisplayList::ItemType::FillPathSegment,
    WebCore::DisplayList::ItemType::FillPath,
    WebCore::DisplayList::ItemType::FillEllipse,
#if ENABLE(VIDEO)
    WebCore::DisplayList::ItemType::PaintFrameForMedia,
#endif
    WebCore::DisplayList::ItemType::StrokeRect,
    WebCore::DisplayList::ItemType::StrokeLine,
#if ENABLE(INLINE_PATH_DATA)
    WebCore::DisplayList::ItemType::StrokeArc,
    WebCore::DisplayList::ItemType::StrokeQuadCurve,
    WebCore::DisplayList::ItemType::StrokeBezierCurve,
#endif
    WebCore::DisplayList::ItemType::StrokePathSegment,
    WebCore::DisplayList::ItemType::StrokePath,
    WebCore::DisplayList::ItemType::StrokeEllipse,
    WebCore::DisplayList::ItemType::ClearRect,
    WebCore::DisplayList::ItemType::BeginTransparencyLayer,
    WebCore::DisplayList::ItemType::EndTransparencyLayer,
#if USE(CG)
    WebCore::DisplayList::ItemType::ApplyStrokePattern,
    WebCore::DisplayList::ItemType::ApplyFillPattern,
#endif
    WebCore::DisplayList::ItemType::ApplyDeviceScaleFactor
    >;

};

} // namespace WTF
