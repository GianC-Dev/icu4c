// © 2018 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

// This file contains one implementation of FormattedValue.
// Other independent implementations should go into their own cpp file for
// better dependency modularization.

#include "unicode/ustring.h"
#include "formattedval_impl.h"
#include "number_types.h"
#include "formatted_string_builder.h"
#include "number_utils.h"
#include "static_unicode_sets.h"
#include "unicode/listformatter.h"

U_NAMESPACE_BEGIN


typedef FormattedStringBuilder::Field Field;


FormattedValueStringBuilderImpl::FormattedValueStringBuilderImpl(Field numericField)
        : fNumericField(numericField) {
}

FormattedValueStringBuilderImpl::~FormattedValueStringBuilderImpl() {
}


UnicodeString FormattedValueStringBuilderImpl::toString(UErrorCode&) const {
    return fString.toUnicodeString();
}

UnicodeString FormattedValueStringBuilderImpl::toTempString(UErrorCode&) const {
    return fString.toTempUnicodeString();
}

Appendable& FormattedValueStringBuilderImpl::appendTo(Appendable& appendable, UErrorCode&) const {
    appendable.appendString(fString.chars(), fString.length());
    return appendable;
}

UBool FormattedValueStringBuilderImpl::nextPosition(ConstrainedFieldPosition& cfpos, UErrorCode& status) const {
    // NOTE: MSVC sometimes complains when implicitly converting between bool and UBool
    return nextPositionImpl(cfpos, fNumericField, status) ? TRUE : FALSE;
}

UBool FormattedValueStringBuilderImpl::nextFieldPosition(FieldPosition& fp, UErrorCode& status) const {
    int32_t rawField = fp.getField();

    if (rawField == FieldPosition::DONT_CARE) {
        return FALSE;
    }

    if (rawField < 0 || rawField >= UNUM_FIELD_COUNT) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return FALSE;
    }

    ConstrainedFieldPosition cfpos;
    cfpos.constrainField(UFIELD_CATEGORY_NUMBER, rawField);
    cfpos.setState(UFIELD_CATEGORY_NUMBER, rawField, fp.getBeginIndex(), fp.getEndIndex());
    if (nextPositionImpl(cfpos, kUndefinedField, status)) {
        fp.setBeginIndex(cfpos.getStart());
        fp.setEndIndex(cfpos.getLimit());
        return TRUE;
    }

    // Special case: fraction should start after integer if fraction is not present
    if (rawField == UNUM_FRACTION_FIELD && fp.getEndIndex() == 0) {
        bool inside = false;
        int32_t i = fString.fZero;
        for (; i < fString.fZero + fString.fLength; i++) {
            if (isIntOrGroup(fString.getFieldPtr()[i]) || fString.getFieldPtr()[i] == Field(UFIELD_CATEGORY_NUMBER, UNUM_DECIMAL_SEPARATOR_FIELD)) {
                inside = true;
            } else if (inside) {
                break;
            }
        }
        fp.setBeginIndex(i - fString.fZero);
        fp.setEndIndex(i - fString.fZero);
    }

    return FALSE;
}

void FormattedValueStringBuilderImpl::getAllFieldPositions(FieldPositionIteratorHandler& fpih,
                                               UErrorCode& status) const {
    ConstrainedFieldPosition cfpos;
    while (nextPositionImpl(cfpos, kUndefinedField, status)) {
        fpih.addAttribute(cfpos.getField(), cfpos.getStart(), cfpos.getLimit());
    }
}

// Signal the end of the string using a field that doesn't exist and that is
// different from kUndefinedField, which is used for "null field".
static constexpr Field kEndField = Field(0xf, 0xf);

bool FormattedValueStringBuilderImpl::nextPositionImpl(ConstrainedFieldPosition& cfpos, Field numericField, UErrorCode& /*status*/) const {
    int32_t fieldStart = -1;
    Field currField = kUndefinedField;
    UFieldCategory spanCategory = UFIELD_CATEGORY_UNDEFINED;
    int32_t spanValue;
    for (int32_t i = fString.fZero + cfpos.getLimit(); i <= fString.fZero + fString.fLength; i++) {
        Field _field = (i < fString.fZero + fString.fLength) ? fString.getFieldPtr()[i] : kEndField;
        // Case 1: currently scanning a field.
        if (currField != kUndefinedField) {
            if (currField != _field) {
                int32_t end = i - fString.fZero;
                // Handle span fields; don't trim them
                if (spanCategory != UFIELD_CATEGORY_UNDEFINED) {
                    cfpos.setState(
                        spanCategory,
                        spanValue,
                        fieldStart,
                        end);
                    return true;
                }
                // Grouping separators can be whitespace; don't throw them out!
                if (isTrimmable(currField)) {
                    end = trimBack(i - fString.fZero);
                }
                if (end <= fieldStart) {
                    // Entire field position is ignorable; skip.
                    fieldStart = -1;
                    currField = kUndefinedField;
                    i--;  // look at this index again
                    continue;
                }
                int32_t start = fieldStart;
                if (isTrimmable(currField)) {
                    start = trimFront(start);
                }
                cfpos.setState(currField.getCategory(), currField.getField(), start, end);
                return true;
            }
            continue;
        }
        // Special case: coalesce the INTEGER if we are pointing at the end of the INTEGER.
        if (cfpos.matchesField(UFIELD_CATEGORY_NUMBER, UNUM_INTEGER_FIELD)
                && i > fString.fZero
                // don't return the same field twice in a row:
                && i - fString.fZero > cfpos.getLimit()
                && isIntOrGroup(fString.getFieldPtr()[i - 1])
                && !isIntOrGroup(_field)) {
            int j = i - 1;
            for (; j >= fString.fZero && isIntOrGroup(fString.getFieldPtr()[j]); j--) {}
            cfpos.setState(
                UFIELD_CATEGORY_NUMBER,
                UNUM_INTEGER_FIELD,
                j - fString.fZero + 1,
                i - fString.fZero);
            return true;
        }
        // Special case: coalesce NUMERIC if we are pointing at the end of the NUMERIC.
        if (numericField != kUndefinedField
                && cfpos.matchesField(numericField.getCategory(), numericField.getField())
                && i > fString.fZero
                // don't return the same field twice in a row:
                && (i - fString.fZero > cfpos.getLimit()
                    || cfpos.getCategory() != numericField.getCategory()
                    || cfpos.getField() != numericField.getField())
                && fString.getFieldPtr()[i - 1].isNumeric()
                && !_field.isNumeric()) {
            // Re-wind to the beginning of the field and then emit it
            int32_t j = i - 1;
            for (; j >= fString.fZero && fString.getFieldPtr()[j].isNumeric(); j--) {}
            cfpos.setState(
                numericField.getCategory(),
                numericField.getField(),
                j - fString.fZero + 1,
                i - fString.fZero);
            return true;
        }
        // Special case: emit normalField if we are pointing at the end of spanField.
        if (i > fString.fZero) {
            auto elementField = fString.getFieldPtr()[i-1];
            if (elementField == Field(UFIELD_CATEGORY_LIST, ULISTFMT_ELEMENT_FIELD)
                    && cfpos.matchesField(elementField.getCategory(), elementField.getField())
                    && (cfpos.getLimit() < i - fString.fZero || cfpos.getCategory() != elementField.getCategory())) {
                // Re-wind to the beginning of the field and then emit it
                int32_t j = i - 1;
                for (; j >= fString.fZero && fString.getFieldPtr()[j] == fString.getFieldPtr()[i-1]; j--) {}
                cfpos.setState(
                    elementField.getCategory(),
                    elementField.getField(),
                    j - fString.fZero + 1,
                    i - fString.fZero);
                return true;
            }
        }
        // Special case: skip over INTEGER; will be coalesced later.
        if (_field == Field(UFIELD_CATEGORY_NUMBER, UNUM_INTEGER_FIELD)) {
            _field = kUndefinedField;
        }
        // Case 2: no field starting at this position.
        if (_field.isUndefined() || _field == kEndField) {
            continue;
        }
        // Case 3: check for field starting at this position
        // Case 3a: Need to add a SpanField
        if (_field == Field(UFIELD_CATEGORY_LIST, ULISTFMT_ELEMENT_FIELD)
                // don't return the same field twice in a row:
                && (i == fString.fZero
                    || fString.getFieldPtr()[i-1].getCategory() != UFIELD_CATEGORY_LIST
                    || fString.getFieldPtr()[i-1].getField() != ULISTFMT_ELEMENT_FIELD)) {
            int64_t si = cfpos.getInt64IterationContext();
            spanValue = spanIndices[si];
            cfpos.setInt64IterationContext(si + 1);
            if (cfpos.matchesField(UFIELD_CATEGORY_LIST_SPAN, spanValue)) {
                spanCategory = UFIELD_CATEGORY_LIST_SPAN;
                fieldStart = i - fString.fZero;
                currField = _field;
                continue;
            }
        }
        // Case 3b: No SpanField or SpanField did not match
        if (cfpos.matchesField(_field.getCategory(), _field.getField())) {
            fieldStart = i - fString.fZero;
            currField = _field;
        }
    }

    U_ASSERT(currField == kUndefinedField);
    return false;
}

void FormattedValueStringBuilderImpl::appendSpanIndex(int32_t position) {
    if (spanIndices.getCapacity() <= position) {
        spanIndices.resize(position * 2);
    }
    spanIndices[position] = position;
}

void FormattedValueStringBuilderImpl::prependSpanIndex(int32_t position) {
    if (spanIndices.getCapacity() <= position) {
        spanIndices.resize(position * 2);
    }
    for (int32_t i = 0; i < position; i++) {
        spanIndices[i+1] = spanIndices[i];
    }
    spanIndices[0] = position;
}

bool FormattedValueStringBuilderImpl::isIntOrGroup(Field field) {
    return field == Field(UFIELD_CATEGORY_NUMBER, UNUM_INTEGER_FIELD)
        || field == Field(UFIELD_CATEGORY_NUMBER, UNUM_GROUPING_SEPARATOR_FIELD);
}

bool FormattedValueStringBuilderImpl::isTrimmable(Field field) {
    return field != Field(UFIELD_CATEGORY_NUMBER, UNUM_GROUPING_SEPARATOR_FIELD)
        && field.getCategory() != UFIELD_CATEGORY_LIST;
}

int32_t FormattedValueStringBuilderImpl::trimBack(int32_t limit) const {
    return unisets::get(unisets::DEFAULT_IGNORABLES)->spanBack(
        fString.getCharPtr() + fString.fZero,
        limit,
        USET_SPAN_CONTAINED);
}

int32_t FormattedValueStringBuilderImpl::trimFront(int32_t start) const {
    return start + unisets::get(unisets::DEFAULT_IGNORABLES)->span(
        fString.getCharPtr() + fString.fZero + start,
        fString.fLength - start,
        USET_SPAN_CONTAINED);
}


U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */
