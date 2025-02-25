/*
 * Copyright (C) 2024 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "FontPlatformData.h"

#include "FontCache.h"
#include "FontCustomPlatformData.h"
#include "FontVariationsSkia.h"
#include "NotImplemented.h"
#include "SkiaHarfBuzzFont.h"
#include <skia/core/SkStream.h>
#include <skia/core/SkTypeface.h>
#include <wtf/Hasher.h>
#include <wtf/VectorHash.h>

namespace WebCore {

FontPlatformData::FontPlatformData(sk_sp<SkTypeface>&& typeface, float size, bool syntheticBold, bool syntheticOblique, FontOrientation orientation, FontWidthVariant widthVariant, TextRenderingMode textRenderingMode, Vector<hb_feature_t>&& features, const FontCustomPlatformData* customPlatformData)
    : FontPlatformData(size, syntheticBold, syntheticOblique, orientation, widthVariant, textRenderingMode, customPlatformData)
{
    m_font = SkFont(typeface, m_size);
    m_font.setEmbolden(m_syntheticBold);
    m_font.setSkewX(m_syntheticOblique ? -SK_Scalar1 / 4 : 0);

    m_hbFont = SkiaHarfBuzzFont::getOrCreate(*m_font.getTypeface());

    m_features = WTFMove(features);
}

bool FontPlatformData::isFixedPitch() const
{
    return m_font.getTypeface()->isFixedPitch();
}

unsigned FontPlatformData::hash() const
{
    // FIXME: do we need to consider m_features for the hash?
    return computeHash(m_font.getTypeface()->uniqueID(), m_widthVariant, m_isHashTableDeletedValue, m_textRenderingMode, m_orientation, m_syntheticBold, m_syntheticOblique);
}

bool FontPlatformData::platformIsEqual(const FontPlatformData& other) const
{

    return SkTypeface::Equal(m_font.getTypeface(), other.skFont().getTypeface()) && m_features == other.m_features;
}

#if !LOG_DISABLED
String FontPlatformData::description() const
{
    return String();
}
#endif

String FontPlatformData::familyName() const
{
    if (auto* typeface = m_font.getTypeface()) {
        SkString familyName;
        typeface->getFamilyName(&familyName);
        return String::fromUTF8(familyName.data());
    }
    return { };
}

RefPtr<SharedBuffer> FontPlatformData::openTypeTable(uint32_t table) const
{
    notImplemented();
    UNUSED_PARAM(table);
    return nullptr;
}

FontPlatformData FontPlatformData::create(const Attributes& data, const FontCustomPlatformData* custom)
{
    ASSERT(custom);
    sk_sp<SkTypeface> typeface = custom->m_typeface;
    Vector<hb_feature_t> features = data.m_features;
    return FontPlatformData(WTFMove(typeface), data.m_size, data.m_syntheticBold, data.m_syntheticOblique, data.m_orientation, data.m_widthVariant, data.m_textRenderingMode, WTFMove(features), custom);
}

FontPlatformData::Attributes FontPlatformData::attributes() const
{
    Attributes result(m_size, m_orientation, m_widthVariant, m_textRenderingMode, m_syntheticBold, m_syntheticOblique);
    result.m_features = m_features;
    return result;
}

hb_font_t* FontPlatformData::hbFont() const
{
    return m_hbFont->scaledFont(*this);
}

#if ENABLE(MATHML)
HbUniquePtr<hb_font_t> FontPlatformData::createOpenTypeMathHarfBuzzFont() const
{
    notImplemented();
    return nullptr;
}
#endif

void FontPlatformData::updateSize(float size)
{
    m_size = size;
    m_font.setSize(m_size);
}

Vector<FontPlatformData::FontVariationAxis> FontPlatformData::variationAxes(ShouldLocalizeAxisNames) const
{
    auto* typeface = m_font.getTypeface();
    if (!typeface)
        return { };

    return WTF::map(defaultFontVariationValues(*typeface), [](auto&& entry) {
        auto& [tag, values] = entry;
        return FontPlatformData::FontVariationAxis { values.axisName, String(tag.data(), tag.size()), values.defaultValue, values.minimumValue, values.maximumValue };
    });
}

} // namespace WebCore
