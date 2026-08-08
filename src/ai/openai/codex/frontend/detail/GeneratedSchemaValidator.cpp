/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/GeneratedSchemaValidator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <regex>
#include <utility>

namespace ai::openai::codex::frontend::detail {

    const Json& generatedProtocolSchema() {
        static const Json schema = Json::parse(
#include "ai/openai/codex/frontend/GeneratedProtocolSchema.inc"
        );
        return schema;
    }

    namespace {

        struct SchemaValidation {
            bool valid = true;
            bool missingRequired = false;
            bool terminal = false;
            bool internalFailure = false;
            std::size_t specificity = 0;
            std::string message;
        };

        struct SchemaValidationContext {
            const Json& root;
            SchemaValidationLimits limits;
            SchemaValidationStatistics* statistics = nullptr;
            std::size_t visits = 0;
        };

        void saturatingIncrement(std::size_t& value) noexcept {
            if (value != std::numeric_limits<std::size_t>::max()) {
                ++value;
            }
        }

        void observeDepth(SchemaValidationContext& context, std::size_t depth) noexcept {
            if (context.statistics != nullptr) {
                context.statistics->maximumDepthObserved = std::max(context.statistics->maximumDepthObserved, depth);
            }
        }

        void incrementStatistic(std::size_t SchemaValidationStatistics::* member, SchemaValidationContext& context) noexcept {
            if (context.statistics != nullptr) {
                saturatingIncrement(context.statistics->*member);
            }
        }

        SchemaValidation schemaFailure(std::string message,
                                       std::size_t specificity,
                                       bool missingRequired = false,
                                       bool terminal = false,
                                       bool internalFailure = false) {
            return SchemaValidation{false, missingRequired, terminal, internalFailure, specificity, std::move(message)};
        }

        SchemaValidation generatedSchemaFailure(std::string message, std::size_t specificity) {
            return schemaFailure(std::move(message), specificity, false, true, true);
        }

        SchemaValidation complexityFailure(SchemaValidationContext& context, std::string_view path, std::size_t specificity) {
            if (context.statistics != nullptr) {
                context.statistics->complexityRejected = true;
            }
            return schemaFailure(std::string(path) + " exceeds the frontend schema validation complexity bound", specificity, false, true);
        }

        bool consumeVisit(SchemaValidationContext& context, std::size_t depth) noexcept {
            observeDepth(context, depth);
            if (depth > context.limits.maximumDepth || context.visits >= context.limits.maximumVisits) {
                return false;
            }
            saturatingIncrement(context.visits);
            if (context.statistics != nullptr) {
                context.statistics->visits = context.visits;
            }
            return true;
        }

        const SchemaValidation& moreSpecificFailure(const SchemaValidation& left, const SchemaValidation& right) {
            if (right.specificity > left.specificity ||
                (right.specificity == left.specificity && right.missingRequired && !left.missingRequired)) {
                return right;
            }
            return left;
        }

        std::string childPath(std::string_view parent, std::string_view member) {
            std::string result(parent);
            result.push_back('.');
            result.append(member);
            return result;
        }

        std::string childPath(std::string_view parent, std::size_t index) {
            return std::string(parent) + '[' + std::to_string(index) + ']';
        }

        std::optional<std::size_t> utf8CodePointWidth(std::string_view text, std::size_t index) noexcept {
            if (index >= text.size()) {
                return std::nullopt;
            }
            const auto lead = static_cast<unsigned char>(text[index]);
            std::size_t width = 0;
            if (lead <= 0x7FU) {
                width = 1;
            } else if (lead >= 0xC2U && lead <= 0xDFU) {
                width = 2;
            } else if (lead >= 0xE0U && lead <= 0xEFU) {
                width = 3;
            } else if (lead >= 0xF0U && lead <= 0xF4U) {
                width = 4;
            } else {
                return std::nullopt;
            }
            if (index + width > text.size()) {
                return std::nullopt;
            }
            for (std::size_t continuation = 1; continuation < width; ++continuation) {
                const auto byte = static_cast<unsigned char>(text[index + continuation]);
                if ((byte & 0xC0U) != 0x80U) {
                    return std::nullopt;
                }
            }
            if (width == 3) {
                const auto second = static_cast<unsigned char>(text[index + 1]);
                if ((lead == 0xE0U && second < 0xA0U) || (lead == 0xEDU && second > 0x9FU)) {
                    return std::nullopt;
                }
            } else if (width == 4) {
                const auto second = static_cast<unsigned char>(text[index + 1]);
                if ((lead == 0xF0U && second < 0x90U) || (lead == 0xF4U && second > 0x8FU)) {
                    return std::nullopt;
                }
            }
            return width;
        }

        std::string boundedDiscriminatorValue(std::string_view value) {
            constexpr std::size_t MaximumRenderedBytes = 128;
            constexpr std::string_view TruncationMarker = "...";
            constexpr char Hex[] = "0123456789abcdef";
            const std::size_t contentLimit = MaximumRenderedBytes - TruncationMarker.size();

            std::string rendered;
            rendered.reserve(MaximumRenderedBytes);
            std::size_t index = 0;
            bool truncated = false;
            while (index < value.size()) {
                const std::optional<std::size_t> width = utf8CodePointWidth(value, index);
                if (!width.has_value()) {
                    truncated = true;
                    break;
                }

                std::string escaped;
                if (*width != 1) {
                    escaped.assign(value.substr(index, *width));
                } else {
                    const auto byte = static_cast<unsigned char>(value[index]);
                    switch (byte) {
                        case '\'':
                            escaped = "\\'";
                            break;
                        case '\\':
                            escaped = "\\\\";
                            break;
                        case '\b':
                            escaped = "\\b";
                            break;
                        case '\f':
                            escaped = "\\f";
                            break;
                        case '\n':
                            escaped = "\\n";
                            break;
                        case '\r':
                            escaped = "\\r";
                            break;
                        case '\t':
                            escaped = "\\t";
                            break;
                        default:
                            if (byte < 0x20U || byte == 0x7FU) {
                                escaped = "\\u00";
                                escaped.push_back(Hex[(byte >> 4U) & 0x0FU]);
                                escaped.push_back(Hex[byte & 0x0FU]);
                            } else {
                                escaped.push_back(static_cast<char>(byte));
                            }
                            break;
                    }
                }
                if (escaped.size() > contentLimit - rendered.size()) {
                    truncated = true;
                    break;
                }
                rendered += escaped;
                index += *width;
            }
            if (index != value.size()) {
                truncated = true;
            }
            if (truncated) {
                rendered += TruncationMarker;
            }
            return rendered;
        }

        std::string discriminatorFailure(std::string_view path, const Json& value) {
            std::string message(path);
            if (value.is_string()) {
                message += " value '";
                message += boundedDiscriminatorValue(value.get_ref<const std::string&>());
                message += "'";
            } else {
                message += " value has JSON type '";
                message += value.type_name();
                message += "'";
            }
            message += " is not a schema-defined discriminator";
            return message;
        }

        const Json* resolveLocalSchemaReference(const Json& root, std::string_view reference) {
            if (reference == "#") {
                return &root;
            }
            if (!reference.starts_with("#/")) {
                return nullptr;
            }
            try {
                return &root.at(Json::json_pointer(std::string(reference.substr(1))));
            } catch (const Json::exception&) {
                return nullptr;
            }
        }

        bool isFiniteIntegral(double value) {
            return std::isfinite(value) && std::fpclassify(std::fmod(value, 1.0)) == FP_ZERO;
        }

        bool matchesSchemaType(const Json& value, std::string_view type) {
            if (type == "object") {
                return value.is_object();
            }
            if (type == "array") {
                return value.is_array();
            }
            if (type == "string") {
                return value.is_string();
            }
            if (type == "integer") {
                if (value.is_number_integer() || value.is_number_unsigned()) {
                    return true;
                }
                return value.is_number_float() && isFiniteIntegral(value.get<double>());
            }
            if (type == "number") {
                return value.is_number();
            }
            if (type == "boolean") {
                return value.is_boolean();
            }
            if (type == "null") {
                return value.is_null();
            }
            return false;
        }

        std::string schemaTypeDescription(const Json& type) {
            if (type.is_string()) {
                return type.get<std::string>();
            }
            if (type.is_array()) {
                std::string description;
                for (const Json& alternative : type) {
                    if (!alternative.is_string()) {
                        continue;
                    }
                    if (!description.empty()) {
                        description += " or ";
                    }
                    description += alternative.get_ref<const std::string&>();
                }
                return description;
            }
            return "a schema-defined type";
        }

        bool matchesAnySchemaType(const Json& value, const Json& type) {
            if (type.is_string()) {
                return matchesSchemaType(value, type.get_ref<const std::string&>());
            }
            if (!type.is_array()) {
                return false;
            }
            return std::any_of(type.begin(), type.end(), [&value](const Json& alternative) {
                return alternative.is_string() && matchesSchemaType(value, alternative.get_ref<const std::string&>());
            });
        }

        std::optional<std::size_t> schemaSize(const Json& schema, std::string_view keyword) {
            const auto value = schema.find(std::string(keyword));
            if (value == schema.end()) {
                return std::nullopt;
            }
            if (value->is_number_unsigned()) {
                const std::uint64_t size = value->get<std::uint64_t>();
                if (size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return static_cast<std::size_t>(size);
                }
            } else if (value->is_number_integer()) {
                const std::int64_t size = value->get<std::int64_t>();
                if (size >= 0 && static_cast<std::uint64_t>(size) <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return static_cast<std::size_t>(size);
                }
            }
            return std::nullopt;
        }

        long double numericValue(const Json& value) {
            if (value.is_number_unsigned()) {
                return static_cast<long double>(value.get<std::uint64_t>());
            }
            if (value.is_number_integer()) {
                return static_cast<long double>(value.get<std::int64_t>());
            }
            return static_cast<long double>(value.get<double>());
        }

        bool fitsSignedIntegerFormat(const Json& value, std::int64_t minimum, std::int64_t maximum) {
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum);
            }
            if (value.is_number_integer()) {
                const std::int64_t number = value.get<std::int64_t>();
                return number >= minimum && number <= maximum;
            }
            if (!value.is_number_float()) {
                return false;
            }
            const double number = value.get<double>();
            if (!isFiniteIntegral(number)) {
                return false;
            }
            if (minimum == std::numeric_limits<std::int64_t>::min() && maximum == std::numeric_limits<std::int64_t>::max()) {
                // INT64_MAX rounds to 2^63 when represented as a double. Use
                // an exclusive power-of-two upper bound so that rounded
                // out-of-range values cannot be accepted.
                return number >= -0x1p63 && number < 0x1p63;
            }
            return number >= static_cast<double>(minimum) && number <= static_cast<double>(maximum);
        }

        bool fitsUnsignedIntegerFormat(const Json& value, std::uint64_t maximum) {
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>() <= maximum;
            }
            if (value.is_number_integer()) {
                const std::int64_t number = value.get<std::int64_t>();
                return number >= 0 && static_cast<std::uint64_t>(number) <= maximum;
            }
            if (!value.is_number_float()) {
                return false;
            }
            const double number = value.get<double>();
            if (!isFiniteIntegral(number) || number < 0.0) {
                return false;
            }
            if (maximum == std::numeric_limits<std::uint64_t>::max()) {
                // UINT64_MAX rounds to 2^64 as a double. Keep that boundary
                // exclusive for the same reason as the signed case above.
                return number < 0x1p64;
            }
            return number <= static_cast<double>(maximum);
        }

        bool isSupportedNumericFormat(std::string_view name) noexcept {
            return name == "int32" || name == "int64" || name == "uint16" || name == "uint32" || name == "uint" || name == "uint64";
        }

        SchemaValidation validateNumericFormatDeclaration(const Json& schema, std::string_view path, std::size_t depth) {
            const auto format = schema.find("format");
            if (format == schema.end()) {
                return {};
            }
            if (!format->is_string()) {
                return generatedSchemaFailure(std::string(path) + " has an invalid generated numeric format", depth);
            }
            const std::string_view name = format->get_ref<const std::string&>();
            if (!isSupportedNumericFormat(name)) {
                return generatedSchemaFailure(std::string(path) + " has an unknown generated numeric format", depth);
            }
            return {};
        }

        SchemaValidation validateNumericFormat(const Json& schema, const Json& value, std::string_view path, std::size_t depth) {
            const auto format = schema.find("format");
            if (format == schema.end() || !value.is_number()) {
                return {};
            }
            const std::string_view name = format->get_ref<const std::string&>();
            bool valid = false;
            if (name == "int32") {
                valid = fitsSignedIntegerFormat(value, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
            } else if (name == "int64") {
                valid = fitsSignedIntegerFormat(value, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
            } else if (name == "uint16") {
                valid = fitsUnsignedIntegerFormat(value, std::numeric_limits<std::uint16_t>::max());
            } else if (name == "uint32") {
                valid = fitsUnsignedIntegerFormat(value, std::numeric_limits<std::uint32_t>::max());
            } else if (name == "uint" || name == "uint64") {
                valid = fitsUnsignedIntegerFormat(value, std::numeric_limits<std::uint64_t>::max());
            }
            return valid ? SchemaValidation{} : schemaFailure(std::string(path) + " is outside the schema numeric format range", depth);
        }

        std::string normalizedSchemaFieldName(std::string_view name) {
            std::string normalized;
            normalized.reserve(name.size());
            for (const unsigned char byte : name) {
                if ((byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
                    (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) || byte == '_') {
                    normalized.push_back(static_cast<char>(byte));
                } else if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
                    normalized.push_back(static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a')));
                }
            }
            return normalized;
        }

        SchemaValidation validateForbiddenSensitiveFieldNames(
            SchemaValidationContext& context, const Json& forbiddenNames, const Json& value, std::string_view path, std::size_t depth) {
            if (!consumeVisit(context, depth)) {
                return complexityFailure(context, path, depth);
            }
            if (!forbiddenNames.is_array() || std::any_of(forbiddenNames.begin(), forbiddenNames.end(), [](const Json& name) {
                    return !name.is_string();
                })) {
                return generatedSchemaFailure(std::string(path) + " has invalid generated sensitive-field metadata", depth);
            }
            if (value.is_object()) {
                for (const auto& [name, member] : value.items()) {
                    const std::string normalized = normalizedSchemaFieldName(name);
                    if (std::any_of(forbiddenNames.begin(), forbiddenNames.end(), [&normalized](const Json& forbidden) {
                            return forbidden.get_ref<const std::string&>() == normalized;
                        })) {
                        return schemaFailure(std::string(path) + " contains a forbidden sensitive field name", depth);
                    }
                    const SchemaValidation nested =
                        validateForbiddenSensitiveFieldNames(context, forbiddenNames, member, childPath(path, name), depth + 1);
                    if (!nested.valid) {
                        return nested;
                    }
                }
            } else if (value.is_array()) {
                for (std::size_t index = 0; index < value.size(); ++index) {
                    const SchemaValidation nested =
                        validateForbiddenSensitiveFieldNames(context, forbiddenNames, value[index], childPath(path, index), depth + 1);
                    if (!nested.valid) {
                        return nested;
                    }
                }
            }
            return {};
        }

        SchemaValidation validateSchemaNode(
            SchemaValidationContext& context, const Json& schema, const Json& value, std::string_view path, std::size_t depth);

        const Json*
        schemaPropertyConstant(SchemaValidationContext& context, const Json& schema, std::string_view property, std::size_t depth = 0) {
            if (depth > context.limits.maximumDepth || !schema.is_object()) {
                return nullptr;
            }
            if (const auto reference = schema.find("$ref"); reference != schema.end() && reference->is_string()) {
                if (const Json* target = resolveLocalSchemaReference(context.root, reference->get_ref<const std::string&>());
                    target != nullptr) {
                    incrementStatistic(&SchemaValidationStatistics::referencesResolved, context);
                    if (const Json* constant = schemaPropertyConstant(context, *target, property, depth + 1); constant != nullptr) {
                        return constant;
                    }
                }
            }
            if (const auto properties = schema.find("properties"); properties != schema.end() && properties->is_object()) {
                if (const auto member = properties->find(std::string(property)); member != properties->end() && member->is_object()) {
                    if (const auto constant = member->find("const"); constant != member->end()) {
                        return &*constant;
                    }
                }
            }
            if (const auto allOf = schema.find("allOf"); allOf != schema.end() && allOf->is_array()) {
                for (const Json& part : *allOf) {
                    if (const Json* constant = schemaPropertyConstant(context, part, property, depth + 1); constant != nullptr) {
                        return constant;
                    }
                }
            }
            return nullptr;
        }

        std::optional<SchemaValidation> validateDiscriminatedAlternative(
            SchemaValidationContext& context, const Json& alternatives, const Json& value, std::string_view path, std::size_t depth) {
            if (!alternatives.is_array() || !value.is_object()) {
                return std::nullopt;
            }
            for (const std::string_view property : {std::string_view{"method"}, std::string_view{"type"}, std::string_view{"kind"}}) {
                const auto discriminator = value.find(std::string(property));
                if (discriminator == value.end()) {
                    continue;
                }
                const Json* matchingAlternative = nullptr;
                bool completeDiscriminator = !alternatives.empty();
                for (const Json& alternative : alternatives) {
                    const Json* constant = schemaPropertyConstant(context, alternative, property);
                    if (constant == nullptr) {
                        completeDiscriminator = false;
                        break;
                    }
                    if (*constant == *discriminator) {
                        if (matchingAlternative != nullptr) {
                            return generatedSchemaFailure(std::string(path) + " has duplicate generated discriminator values", depth);
                        }
                        matchingAlternative = &alternative;
                    }
                }
                if (!completeDiscriminator) {
                    continue;
                }
                incrementStatistic(&SchemaValidationStatistics::discriminatorFastPaths, context);
                if (matchingAlternative == nullptr) {
                    return schemaFailure(discriminatorFailure(childPath(path, property), *discriminator), depth + 1);
                }
                incrementStatistic(&SchemaValidationStatistics::alternativesEvaluated, context);
                return validateSchemaNode(context, *matchingAlternative, value, path, depth + 1);
            }
            return std::nullopt;
        }

        SchemaValidation validateSchemaAlternatives(SchemaValidationContext& context,
                                                    const Json& alternatives,
                                                    const Json& value,
                                                    std::string_view path,
                                                    std::size_t depth,
                                                    bool requireExactlyOne) {
            if (!alternatives.is_array()) {
                return generatedSchemaFailure(std::string(path) + " has an invalid generated alternative schema", depth);
            }

            if (requireExactlyOne) {
                if (std::optional<SchemaValidation> discriminated =
                        validateDiscriminatedAlternative(context, alternatives, value, path, depth);
                    discriminated.has_value()) {
                    return *discriminated;
                }
            }

            std::size_t matches = 0;
            SchemaValidation bestFailure = schemaFailure(std::string(path) + " does not match any schema alternative", depth);
            for (const Json& alternative : alternatives) {
                incrementStatistic(&SchemaValidationStatistics::alternativesEvaluated, context);
                const SchemaValidation validation = validateSchemaNode(context, alternative, value, path, depth + 1);
                if (validation.terminal) {
                    return validation;
                }
                if (validation.valid) {
                    ++matches;
                    if (!requireExactlyOne) {
                        return {};
                    }
                } else {
                    bestFailure = moreSpecificFailure(bestFailure, validation);
                }
            }
            if ((!requireExactlyOne && matches != 0) || (requireExactlyOne && matches == 1)) {
                return {};
            }
            if (requireExactlyOne && matches > 1) {
                return schemaFailure(std::string(path) + " matches more than one exclusive schema alternative", depth);
            }
            return bestFailure;
        }

        std::optional<std::size_t> utf8CharacterCount(std::string_view text) {
            std::size_t characters = 0;
            for (std::size_t index = 0; index < text.size();) {
                const std::optional<std::size_t> width = utf8CodePointWidth(text, index);
                if (!width.has_value()) {
                    return std::nullopt;
                }
                index += *width;
                ++characters;
            }
            return characters;
        }

        SchemaValidation validateSchemaNode(
            SchemaValidationContext& context, const Json& schema, const Json& value, std::string_view path, std::size_t depth) {
            if (!consumeVisit(context, depth)) {
                return complexityFailure(context, path, depth);
            }
            if (schema.is_boolean()) {
                return schema.get<bool>() ? SchemaValidation{}
                                          : schemaFailure(std::string(path) + " is rejected by the generated schema", depth);
            }
            if (!schema.is_object()) {
                return generatedSchemaFailure(std::string(path) + " has an invalid generated schema", depth);
            }

            const SchemaValidation formatDeclaration = validateNumericFormatDeclaration(schema, path, depth);
            if (!formatDeclaration.valid) {
                return formatDeclaration;
            }

            bool optimizedSafePropertyName = false;
            if (const auto forbiddenNames = schema.find("x-aisuite-forbiddenNormalizedPropertyNames"); forbiddenNames != schema.end()) {
                if (!value.is_string() || !forbiddenNames->is_array() ||
                    std::any_of(forbiddenNames->begin(), forbiddenNames->end(), [](const Json& name) {
                        return !name.is_string();
                    })) {
                    return generatedSchemaFailure(std::string(path) + " has invalid generated property-name metadata", depth);
                }
                const std::string normalized = normalizedSchemaFieldName(value.get_ref<const std::string&>());
                if (std::any_of(forbiddenNames->begin(), forbiddenNames->end(), [&normalized](const Json& forbidden) {
                        return forbidden.get_ref<const std::string&>() == normalized;
                    })) {
                    return schemaFailure(std::string(path) + " contains a forbidden sensitive field name", depth);
                }
                optimizedSafePropertyName = true;
            }

            if (const auto reference = schema.find("$ref"); reference != schema.end()) {
                if (!reference->is_string()) {
                    return generatedSchemaFailure(std::string(path) + " has an invalid generated schema reference", depth);
                }
                const Json* target = resolveLocalSchemaReference(context.root, reference->get_ref<const std::string&>());
                if (target == nullptr) {
                    return generatedSchemaFailure(std::string(path) + " has an unresolved generated schema reference", depth);
                }
                incrementStatistic(&SchemaValidationStatistics::referencesResolved, context);
                const SchemaValidation referenced = validateSchemaNode(context, *target, value, path, depth + 1);
                if (!referenced.valid) {
                    return referenced;
                }
            }

            if (const auto allOf = schema.find("allOf"); allOf != schema.end()) {
                if (!allOf->is_array()) {
                    return generatedSchemaFailure(std::string(path) + " has an invalid generated allOf schema", depth);
                }
                for (const Json& alternative : *allOf) {
                    const SchemaValidation validation = validateSchemaNode(context, alternative, value, path, depth + 1);
                    if (!validation.valid) {
                        return validation;
                    }
                }
            }
            if (const auto anyOf = schema.find("anyOf"); anyOf != schema.end()) {
                const SchemaValidation validation = validateSchemaAlternatives(context, *anyOf, value, path, depth + 1, false);
                if (!validation.valid) {
                    return validation;
                }
            }
            if (const auto oneOf = schema.find("oneOf"); oneOf != schema.end()) {
                const SchemaValidation validation = validateSchemaAlternatives(context, *oneOf, value, path, depth + 1, true);
                if (!validation.valid) {
                    return validation;
                }
            }
            if (const auto negated = schema.find("not"); negated != schema.end() && !optimizedSafePropertyName) {
                const SchemaValidation validation = validateSchemaNode(context, *negated, value, path, depth + 1);
                if (validation.terminal) {
                    return validation;
                }
                if (validation.valid) {
                    return schemaFailure(std::string(path) + " matches a prohibited schema", depth);
                }
            }

            if (const auto type = schema.find("type"); type != schema.end() && !matchesAnySchemaType(value, *type)) {
                return schemaFailure(std::string(path) + " must be " + schemaTypeDescription(*type), depth);
            }
            if (const auto constant = schema.find("const"); constant != schema.end() && value != *constant) {
                return schemaFailure(std::string(path) + " does not match the schema constant", depth);
            }
            if (const auto enumeration = schema.find("enum"); enumeration != schema.end()) {
                if (!enumeration->is_array() || std::find(enumeration->begin(), enumeration->end(), value) == enumeration->end()) {
                    return schemaFailure(std::string(path) + " is not one of the schema-defined values", depth);
                }
            }

            if (const auto forbiddenNames = schema.find("x-aisuite-sensitiveFieldNamesForbidden"); forbiddenNames != schema.end()) {
                const SchemaValidation validation = validateForbiddenSensitiveFieldNames(context, *forbiddenNames, value, path, depth + 1);
                if (!validation.valid) {
                    return validation;
                }
            }

            if (value.is_object()) {
                if (const auto propertyNames = schema.find("propertyNames"); propertyNames != schema.end()) {
                    for (const auto& [name, member] : value.items()) {
                        (void) member;
                        const SchemaValidation validation =
                            validateSchemaNode(context, *propertyNames, Json(name), childPath(path, name), depth + 1);
                        if (!validation.valid) {
                            return validation;
                        }
                    }
                }
                if (const auto properties = schema.find("properties"); properties != schema.end()) {
                    if (!properties->is_object()) {
                        return generatedSchemaFailure(std::string(path) + " has an invalid generated property schema", depth);
                    }
                    for (const auto& [name, propertySchema] : properties->items()) {
                        const auto member = value.find(name);
                        if (member == value.end()) {
                            continue;
                        }
                        const SchemaValidation validation =
                            validateSchemaNode(context, propertySchema, *member, childPath(path, name), depth + 1);
                        if (!validation.valid) {
                            return validation;
                        }
                    }
                }
                if (const auto additional = schema.find("additionalProperties"); additional != schema.end() && additional->is_object()) {
                    const auto properties = schema.find("properties");
                    for (const auto& [name, member] : value.items()) {
                        if (properties != schema.end() && properties->is_object() && properties->contains(name)) {
                            continue;
                        }
                        const SchemaValidation validation =
                            validateSchemaNode(context, *additional, member, childPath(path, name), depth + 1);
                        if (!validation.valid) {
                            return validation;
                        }
                    }
                } else if (additional != schema.end() && !additional->is_boolean()) {
                    return generatedSchemaFailure(std::string(path) + " has an invalid generated additional-property schema", depth);
                }
                // Validate supplied fields before reporting an absent one. This
                // lets tagged-union discriminator mismatches remain InvalidField
                // while a matching alternative with an omitted payload remains
                // MissingField.
                if (const auto required = schema.find("required"); required != schema.end()) {
                    if (!required->is_array()) {
                        return generatedSchemaFailure(std::string(path) + " has an invalid generated required-field schema", depth);
                    }
                    for (const Json& field : *required) {
                        if (!field.is_string()) {
                            return generatedSchemaFailure(std::string(path) + " has an invalid generated required-field name", depth);
                        }
                        const std::string& name = field.get_ref<const std::string&>();
                        if (!value.contains(name)) {
                            return schemaFailure(childPath(path, name) + " is required", depth + 1, true);
                        }
                    }
                }
                if (const auto minimum = schemaSize(schema, "minProperties"); minimum.has_value() && value.size() < *minimum) {
                    return schemaFailure(std::string(path) + " has too few properties", depth);
                }
                if (const auto maximum = schemaSize(schema, "maxProperties"); maximum.has_value() && value.size() > *maximum) {
                    return schemaFailure(std::string(path) + " has too many properties", depth);
                }
                // Unknown non-conflicting fields are additive v1 extensions,
                // including where the upstream provider schema is closed.
                // Deliberately do not enforce additionalProperties=false.
            }

            if (value.is_array()) {
                if (const auto minimum = schemaSize(schema, "minItems"); minimum.has_value() && value.size() < *minimum) {
                    return schemaFailure(std::string(path) + " has too few items", depth);
                }
                if (const auto maximum = schemaSize(schema, "maxItems"); maximum.has_value() && value.size() > *maximum) {
                    return schemaFailure(std::string(path) + " has too many items", depth);
                }
                if (const auto items = schema.find("items"); items != schema.end()) {
                    for (std::size_t index = 0; index < value.size(); ++index) {
                        const SchemaValidation validation =
                            validateSchemaNode(context, *items, value[index], childPath(path, index), depth + 1);
                        if (!validation.valid) {
                            return validation;
                        }
                    }
                }
                if (const auto unique = schema.find("uniqueItems"); unique != schema.end() && unique->is_boolean() && unique->get<bool>()) {
                    for (std::size_t left = 0; left < value.size(); ++left) {
                        for (std::size_t right = left + 1; right < value.size(); ++right) {
                            if (!consumeVisit(context, depth)) {
                                return complexityFailure(context, path, depth);
                            }
                            incrementStatistic(&SchemaValidationStatistics::uniqueItemComparisons, context);
                            if (value[left] == value[right]) {
                                return schemaFailure(std::string(path) + " contains duplicate items", depth);
                            }
                        }
                    }
                }
            }

            if (value.is_string()) {
                const std::string& text = value.get_ref<const std::string&>();
                const std::optional<std::size_t> characterCount = utf8CharacterCount(text);
                if (!characterCount.has_value()) {
                    return schemaFailure(std::string(path) + " must contain valid UTF-8", depth);
                }
                if (const auto minimum = schemaSize(schema, "minLength"); minimum.has_value() && *characterCount < *minimum) {
                    return schemaFailure(std::string(path) + " is shorter than the schema minimum", depth);
                }
                if (const auto maximum = schemaSize(schema, "maxLength"); maximum.has_value() && *characterCount > *maximum) {
                    return schemaFailure(std::string(path) + " exceeds the schema length bound", depth);
                }
                if (const auto pattern = schema.find("pattern"); pattern != schema.end()) {
                    if (!pattern->is_string()) {
                        return generatedSchemaFailure(std::string(path) + " has an invalid generated regular expression", depth);
                    }
                    incrementStatistic(&SchemaValidationStatistics::regularExpressionsEvaluated, context);
                    if (!std::regex_search(text, std::regex(pattern->get<std::string>()))) {
                        return schemaFailure(std::string(path) + " does not match the schema pattern", depth);
                    }
                }
            }

            if (value.is_number()) {
                const long double number = numericValue(value);
                if (!std::isfinite(number)) {
                    return schemaFailure(std::string(path) + " must be a finite number", depth);
                }
                if (const auto minimum = schema.find("minimum");
                    minimum != schema.end() && minimum->is_number() && number < numericValue(*minimum)) {
                    return schemaFailure(std::string(path) + " is below the schema minimum", depth);
                }
                if (const auto maximum = schema.find("maximum");
                    maximum != schema.end() && maximum->is_number() && number > numericValue(*maximum)) {
                    return schemaFailure(std::string(path) + " exceeds the schema maximum", depth);
                }
                const SchemaValidation format = validateNumericFormat(schema, value, path, depth);
                if (!format.valid) {
                    return format;
                }
            }

            if (const auto condition = schema.find("if"); condition != schema.end()) {
                const SchemaValidation conditionMatches = validateSchemaNode(context, *condition, value, path, depth + 1);
                if (conditionMatches.terminal) {
                    return conditionMatches;
                }
                const char* branchName = conditionMatches.valid ? "then" : "else";
                if (const auto branch = schema.find(branchName); branch != schema.end()) {
                    const SchemaValidation validation = validateSchemaNode(context, *branch, value, path, depth + 1);
                    if (!validation.valid) {
                        return validation;
                    }
                }
            }
            return {};
        }

    } // namespace

    GeneratedSchemaValidation validateGeneratedSchema(const Json& root,
                                                      std::string_view reference,
                                                      const Json& value,
                                                      std::string_view valueName,
                                                      SchemaValidationLimits limits,
                                                      SchemaValidationStatistics* statistics) noexcept {
        try {
            if (statistics != nullptr) {
                *statistics = {};
            }
            SchemaValidationContext context{root, limits, statistics};
            const Json* schema = resolveLocalSchemaReference(root, reference);
            if (schema == nullptr) {
                return GeneratedSchemaValidation{false, false, true, "generated frontend schema reference is unavailable"};
            }
            incrementStatistic(&SchemaValidationStatistics::referencesResolved, context);
            const SchemaValidation validation = validateSchemaNode(context, *schema, value, valueName, 0);
            return GeneratedSchemaValidation{validation.valid, validation.missingRequired, validation.internalFailure, validation.message};
        } catch (const std::regex_error&) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema contains an invalid regular expression"};
        } catch (const std::exception&) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema validation raised a local exception"};
        } catch (...) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema validation raised an unknown local exception"};
        }
    }

    GeneratedSchemaValidation validateGeneratedSchemaNodeForTest(const Json& root,
                                                                 const Json& schema,
                                                                 const Json& value,
                                                                 std::string_view valueName,
                                                                 SchemaValidationLimits limits,
                                                                 SchemaValidationStatistics* statistics) noexcept {
        try {
            if (statistics != nullptr) {
                *statistics = {};
            }
            SchemaValidationContext context{root, limits, statistics};
            const SchemaValidation validation = validateSchemaNode(context, schema, value, valueName, 0);
            return GeneratedSchemaValidation{validation.valid, validation.missingRequired, validation.internalFailure, validation.message};
        } catch (const std::regex_error&) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema contains an invalid regular expression"};
        } catch (const std::exception&) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema validation raised a local exception"};
        } catch (...) {
            return GeneratedSchemaValidation{false, false, true, "generated frontend schema validation raised an unknown local exception"};
        }
    }

} // namespace ai::openai::codex::frontend::detail
