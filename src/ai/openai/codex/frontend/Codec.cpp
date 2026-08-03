/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"

#include "ai/openai/codex/frontend/Protocol.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <regex>
#include <type_traits>

namespace ai::openai::codex::frontend {

    namespace {

        struct ValidationFailure {
            CodecError error;
        };

        [[noreturn]] void
        fail(ErrorCode code, std::string message, bool closeConnection = false, std::vector<std::uint32_t> supportedVersions = {}) {
            throw ValidationFailure{
                CodecError{code, std::move(message), closeConnection, std::move(supportedVersions), std::nullopt, std::nullopt}};
        }

        template <typename T, typename Function>
        CodecResult<T> guard(Function&& function, const Json* requestSource = nullptr) noexcept {
            try {
                return CodecResult<T>(function());
            } catch (ValidationFailure& failure) {
                if (!failure.error.requestId.has_value() && requestSource != nullptr && requestSource->is_object()) {
                    const auto requestId = requestSource->find("requestId");
                    if (requestId != requestSource->end() && requestId->is_string()) {
                        failure.error.requestId = requestId->get<std::string>();
                    }
                }
                return CodecResult<T>(std::move(failure.error));
            } catch (const std::exception& exception) {
                return CodecResult<T>(CodecError{ErrorCode::InternalError,
                                                 std::string("frontend protocol codec failure: ") + exception.what(),
                                                 false,
                                                 {},
                                                 std::nullopt,
                                                 std::nullopt});
            } catch (...) {
                return CodecResult<T>(CodecError{ErrorCode::InternalError,
                                                 "frontend protocol codec failure: unknown local exception",
                                                 false,
                                                 {},
                                                 std::nullopt,
                                                 std::nullopt});
            }
        }

        const Json& generatedProtocolSchema() {
            static const Json schema = Json::parse(
#include "ai/openai/codex/frontend/GeneratedProtocolSchema.inc"
            );
            return schema;
        }

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
            std::size_t visits = 0;
        };

        constexpr std::size_t MaximumSchemaValidationDepth = 128;
        constexpr std::size_t MaximumSchemaValidationVisits = 4'000'000;

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

        SchemaValidation complexityFailure(std::string_view path, std::size_t specificity) {
            return schemaFailure(std::string(path) + " exceeds the frontend schema validation complexity bound", specificity, false, true);
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
            if (value.is_number_integer()) {
                const std::int64_t number = value.get<std::int64_t>();
                return number >= minimum && number <= maximum;
            }
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum);
            }
            if (!value.is_number_float()) {
                return false;
            }
            const double number = value.get<double>();
            return isFiniteIntegral(number) && static_cast<long double>(number) >= static_cast<long double>(minimum) &&
                   static_cast<long double>(number) <= static_cast<long double>(maximum);
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
            return isFiniteIntegral(number) && number >= 0.0 && static_cast<long double>(number) <= static_cast<long double>(maximum);
        }

        SchemaValidation validateNumericFormat(const Json& schema, const Json& value, std::string_view path, std::size_t depth) {
            const auto format = schema.find("format");
            if (format == schema.end() || !value.is_number()) {
                return {};
            }
            if (!format->is_string()) {
                return generatedSchemaFailure(std::string(path) + " has an invalid generated numeric format", depth);
            }
            const std::string_view name = format->get_ref<const std::string&>();
            bool valid = true;
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
            ++context.visits;
            if (depth > MaximumSchemaValidationDepth || context.visits > MaximumSchemaValidationVisits) {
                return complexityFailure(path, depth);
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

        const Json* schemaPropertyConstant(const Json& root, const Json& schema, std::string_view property, std::size_t depth = 0) {
            if (depth > MaximumSchemaValidationDepth || !schema.is_object()) {
                return nullptr;
            }
            if (const auto reference = schema.find("$ref"); reference != schema.end() && reference->is_string()) {
                if (const Json* target = resolveLocalSchemaReference(root, reference->get_ref<const std::string&>()); target != nullptr) {
                    if (const Json* constant = schemaPropertyConstant(root, *target, property, depth + 1); constant != nullptr) {
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
                    if (const Json* constant = schemaPropertyConstant(root, part, property, depth + 1); constant != nullptr) {
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
                    const Json* constant = schemaPropertyConstant(context.root, alternative, property);
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
                if (matchingAlternative == nullptr) {
                    return schemaFailure(childPath(path, property) + " is not a schema-defined discriminator", depth + 1);
                }
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
                index += width;
                ++characters;
            }
            return characters;
        }

        SchemaValidation validateSchemaNode(
            SchemaValidationContext& context, const Json& schema, const Json& value, std::string_view path, std::size_t depth) {
            ++context.visits;
            if (depth > MaximumSchemaValidationDepth || context.visits > MaximumSchemaValidationVisits) {
                return complexityFailure(path, depth);
            }
            if (schema.is_boolean()) {
                return schema.get<bool>() ? SchemaValidation{}
                                          : schemaFailure(std::string(path) + " is rejected by the generated schema", depth);
            }
            if (!schema.is_object()) {
                return generatedSchemaFailure(std::string(path) + " has an invalid generated schema", depth);
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
                            ++context.visits;
                            if (context.visits > MaximumSchemaValidationVisits) {
                                return complexityFailure(path, depth);
                            }
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
                if (const auto pattern = schema.find("pattern");
                    pattern != schema.end() && pattern->is_string() && !std::regex_search(text, std::regex(pattern->get<std::string>()))) {
                    return schemaFailure(std::string(path) + " does not match the schema pattern", depth);
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

        void validateGeneratedSchema(std::string_view reference, const Json& value, std::string_view valueName) {
            const Json& root = generatedProtocolSchema();
            const Json* schema = resolveLocalSchemaReference(root, reference);
            if (schema == nullptr) {
                fail(ErrorCode::InternalError, "generated frontend schema reference is unavailable");
            }
            SchemaValidationContext context{root};
            const SchemaValidation validation = validateSchemaNode(context, *schema, value, valueName, 0);
            if (!validation.valid) {
                fail(validation.internalFailure ? ErrorCode::InternalError
                                                : (validation.missingRequired ? ErrorCode::MissingField : ErrorCode::InvalidField),
                     std::string(valueName) + " does not satisfy the generated frontend schema: " + validation.message);
            }
        }

        const Json& requireObject(const Json& value, std::string_view field = "message") {
            if (!value.is_object()) {
                fail(ErrorCode::InvalidField, std::string(field) + " must be an object");
            }
            return value;
        }

        const Json& requireMember(const Json& object, std::string_view field) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                fail(ErrorCode::MissingField, "missing required field '" + std::string(field) + "'");
            }
            return *member;
        }

        std::string requireString(const Json& object, std::string_view field, bool nonEmpty = true) {
            const Json& value = requireMember(object, field);
            if (!value.is_string()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be a string");
            }
            std::string result = value.get<std::string>();
            if (nonEmpty && result.empty()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must not be empty");
            }
            return result;
        }

        std::optional<std::string> optionalString(const Json& object, std::string_view field, bool nonEmpty = true) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                return std::nullopt;
            }
            if (!member->is_string()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be a string");
            }
            std::string result = member->get<std::string>();
            if (nonEmpty && result.empty()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must not be empty");
            }
            return result;
        }

        std::optional<bool> optionalBool(const Json& object, std::string_view field) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                return std::nullopt;
            }
            if (!member->is_boolean()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be a boolean");
            }
            return member->get<bool>();
        }

        bool requireBool(const Json& object, std::string_view field) {
            const Json& value = requireMember(object, field);
            if (!value.is_boolean()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be a boolean");
            }
            return value.get<bool>();
        }

        std::uint64_t unsignedInteger(const Json& value, std::string_view field) {
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>();
            }
            if (value.is_number_integer()) {
                const std::int64_t signedValue = value.get<std::int64_t>();
                if (signedValue >= 0) {
                    return static_cast<std::uint64_t>(signedValue);
                }
            }
            fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be a non-negative integer");
        }

        SequenceNumber requireSequence(const Json& object, std::string_view field) {
            return SequenceNumber(unsignedInteger(requireMember(object, field), field));
        }

        std::optional<SequenceNumber> optionalSequence(const Json& object, std::string_view field) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                return std::nullopt;
            }
            return SequenceNumber(unsignedInteger(*member, field));
        }

        std::int64_t requireSignedInteger(const Json& object, std::string_view field) {
            const Json& value = requireMember(object, field);
            if (value.is_number_unsigned()) {
                const std::uint64_t unsignedValue = value.get<std::uint64_t>();
                if (unsignedValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    return static_cast<std::int64_t>(unsignedValue);
                }
            } else if (value.is_number_integer()) {
                return value.get<std::int64_t>();
            }
            fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be an integer in the signed 64-bit range");
        }

        std::optional<std::uint32_t> optionalUint32(const Json& object, std::string_view field) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                return std::nullopt;
            }
            const std::uint64_t value = unsignedInteger(*member, field);
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' exceeds the unsigned 32-bit range");
            }
            return static_cast<std::uint32_t>(value);
        }

        bool isKnown(std::string_view key, std::initializer_list<std::string_view> known) {
            return std::find(known.begin(), known.end(), key) != known.end();
        }

        Json extensionsOf(const Json& object, std::initializer_list<std::string_view> known) {
            Json extensions = Json::object();
            for (const auto& [key, value] : object.items()) {
                if (!isKnown(key, known)) {
                    extensions[key] = value;
                }
            }
            return extensions;
        }

        Json withExtensions(const Json& extensions) {
            if (!extensions.is_object()) {
                fail(ErrorCode::InvalidField, "extensions must be an object");
            }
            return extensions;
        }

        std::string validateEnvelope(const Json& message) {
            requireObject(message);
            const std::string protocol = requireString(message, "protocol");
            if (protocol != ProtocolIdentity) {
                fail(ErrorCode::WrongProtocol, "unsupported protocol identity '" + protocol + "'", true);
            }

            const std::uint64_t version = unsignedInteger(requireMember(message, "version"), "version");
            if (version != ProtocolVersion) {
                fail(ErrorCode::UnsupportedVersion,
                     "unsupported frontend protocol version " + std::to_string(version),
                     true,
                     {SupportedProtocolVersions.begin(), SupportedProtocolVersions.end()});
            }
            return requireString(message, "kind");
        }

        std::vector<std::string> requireStringArray(const Json& object, std::string_view field, bool allowEmpty) {
            const Json& value = requireMember(object, field);
            if (!value.is_array()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be an array");
            }
            if (!allowEmpty && value.empty()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must not be empty");
            }
            std::vector<std::string> result;
            result.reserve(value.size());
            for (const Json& element : value) {
                if (!element.is_string()) {
                    fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must contain only strings");
                }
                result.push_back(element.get<std::string>());
            }
            return result;
        }

        std::optional<std::vector<std::string>> optionalStringArray(const Json& object, std::string_view field) {
            if (!object.contains(std::string(field))) {
                return std::nullopt;
            }
            return requireStringArray(object, field, true);
        }

        std::optional<std::vector<FrontendCapability>> optionalCapabilities(const Json& object, std::string_view field) {
            const auto encoded = optionalStringArray(object, field);
            if (!encoded.has_value()) {
                return std::nullopt;
            }
            std::vector<FrontendCapability> capabilities;
            capabilities.reserve(encoded->size());
            for (const std::string& value : *encoded) {
                const auto capability = frontendCapabilityFromString(value);
                if (!capability.has_value()) {
                    fail(ErrorCode::InvalidField, "unknown frontend capability '" + value + "'");
                }
                if (std::find(capabilities.begin(), capabilities.end(), *capability) != capabilities.end()) {
                    fail(ErrorCode::InvalidField, "duplicate frontend capability '" + value + "'");
                }
                capabilities.push_back(*capability);
            }
            return capabilities;
        }

        std::vector<std::string> encodeCapabilities(const std::vector<FrontendCapability>& capabilities) {
            std::vector<std::string> encoded;
            encoded.reserve(capabilities.size());
            for (const FrontendCapability capability : capabilities) {
                const std::string value(toString(capability));
                if (std::find(encoded.begin(), encoded.end(), value) != encoded.end()) {
                    fail(ErrorCode::InvalidField, "duplicate frontend capability '" + value + "'");
                }
                encoded.push_back(value);
            }
            return encoded;
        }

        std::optional<std::vector<FrontendMethod>> optionalMethods(const Json& object, std::string_view field) {
            const auto encoded = optionalStringArray(object, field);
            if (!encoded.has_value()) {
                return std::nullopt;
            }
            for (const FrontendMethod& method : *encoded) {
                if (!generated::definedMethodFromString(method).has_value()) {
                    fail(ErrorCode::InvalidField, "unknown frontend method '" + method + "'");
                }
            }
            for (std::size_t index = 0; index < encoded->size(); ++index) {
                if (std::find(encoded->begin() + static_cast<std::ptrdiff_t>(index + 1), encoded->end(), (*encoded)[index]) !=
                    encoded->end()) {
                    fail(ErrorCode::InvalidField, "duplicate frontend method '" + (*encoded)[index] + "'");
                }
            }
            return encoded;
        }

        std::vector<std::string> encodeMethods(const std::vector<FrontendMethod>& methods) {
            Json wrapper{{"methods", methods}};
            const auto validated = optionalMethods(wrapper, "methods");
            return validated.value_or(std::vector<std::string>{});
        }

        CapabilityAdvertisement decodeCapabilityAdvertisement(const Json& encoded) {
            requireObject(encoded, "capabilities");
            const auto defined = optionalCapabilities(encoded, "defined");
            const auto implemented = optionalCapabilities(encoded, "implemented");
            const auto permitted = optionalCapabilities(encoded, "permitted");
            if (!defined.has_value() || !implemented.has_value() || !permitted.has_value()) {
                fail(ErrorCode::MissingField, "capability advertisement requires defined, implemented, and permitted");
            }
            return CapabilityAdvertisement{
                *defined, *implemented, *permitted, extensionsOf(encoded, {"defined", "implemented", "permitted"})};
        }

        Json encodeCapabilityAdvertisement(const CapabilityAdvertisement& advertisement) {
            Json encoded = withExtensions(advertisement.extensions);
            encoded["defined"] = encodeCapabilities(advertisement.defined);
            encoded["implemented"] = encodeCapabilities(advertisement.implemented);
            encoded["permitted"] = encodeCapabilities(advertisement.permitted);
            return encoded;
        }

        TurnInput decodeTurnInput(const Json& input) {
            requireObject(input, "turn input");
            const std::string type = requireString(input, "type");
            if (type == "text") {
                return TextInput{requireString(input, "text", false), extensionsOf(input, {"type", "text"})};
            }
            if (type == "image") {
                return ImageUrlInput{
                    requireString(input, "url"), optionalString(input, "detail"), extensionsOf(input, {"type", "url", "detail"})};
            }
            if (type == "localImage") {
                return LocalImageInput{
                    requireString(input, "path"), optionalString(input, "detail"), extensionsOf(input, {"type", "path", "detail"})};
            }
            if (type == "skill") {
                return SkillInput{
                    requireString(input, "name"), requireString(input, "path"), extensionsOf(input, {"type", "name", "path"})};
            }
            if (type == "mention") {
                return MentionInput{
                    requireString(input, "name"), requireString(input, "path"), extensionsOf(input, {"type", "name", "path"})};
            }
            fail(ErrorCode::InvalidField, "unsupported turn input type '" + type + "'");
        }

        SandboxPolicy decodeSandboxPolicy(const Json& encoded) {
            requireObject(encoded, "sandboxPolicy");
            SandboxPolicy policy;
            policy.type = requireString(encoded, "type");
            if (policy.type != "dangerFullAccess" && policy.type != "readOnly" && policy.type != "externalSandbox" &&
                policy.type != "workspaceWrite") {
                fail(ErrorCode::InvalidField, "unsupported sandbox policy type '" + policy.type + "'");
            }

            const auto networkAccess = encoded.find("networkAccess");
            if (networkAccess != encoded.end()) {
                if (policy.type == "externalSandbox") {
                    if (!networkAccess->is_string() || networkAccess->get_ref<const std::string&>().empty()) {
                        fail(ErrorCode::InvalidField, "external sandbox field 'networkAccess' must be a non-empty string");
                    }
                    policy.networkAccess = networkAccess->get<std::string>();
                } else {
                    if (!networkAccess->is_boolean()) {
                        fail(ErrorCode::InvalidField, "sandbox field 'networkAccess' must be a boolean");
                    }
                    policy.networkAccess = networkAccess->get<bool>();
                }
            }
            const auto roots = encoded.find("writableRoots");
            if (roots != encoded.end()) {
                if (policy.type != "workspaceWrite") {
                    fail(ErrorCode::InvalidField, "field 'writableRoots' is only valid for workspaceWrite sandbox policy");
                }
                policy.writableRoots = requireStringArray(encoded, "writableRoots", true);
            }
            policy.excludeTmpdirEnvVar = optionalBool(encoded, "excludeTmpdirEnvVar");
            policy.excludeSlashTmp = optionalBool(encoded, "excludeSlashTmp");
            if (policy.type != "workspaceWrite" && (policy.excludeTmpdirEnvVar.has_value() || policy.excludeSlashTmp.has_value())) {
                fail(ErrorCode::InvalidField, "temporary-directory exclusions are only valid for workspaceWrite sandbox policy");
            }
            policy.extensions = extensionsOf(encoded, {"type", "networkAccess", "writableRoots", "excludeTmpdirEnvVar", "excludeSlashTmp"});
            return policy;
        }

        CommandParameters decodeParameters(CommandMethod methodValue, const Json& params) {
            requireObject(params, "params");
            switch (methodValue) {
                case CommandMethod::ControllerAcquire:
                    return ControllerAcquire{};
                case CommandMethod::ControllerRelease:
                    return ControllerRelease{};
                case CommandMethod::SnapshotGet:
                    return SnapshotGet{};
                case CommandMethod::EventsReplay:
                    return ReplayAfter{requireSequence(params, "after")};
                case CommandMethod::ThreadStart:
                    return ThreadStart{optionalString(params, "cwd"),
                                       optionalString(params, "model"),
                                       optionalString(params, "modelProvider"),
                                       optionalString(params, "approvalPolicy"),
                                       optionalString(params, "sandboxMode"),
                                       optionalBool(params, "ephemeral")};
                case CommandMethod::ThreadResume:
                    return ThreadResume{requireString(params, "threadId"),
                                        optionalString(params, "cwd"),
                                        optionalString(params, "model"),
                                        optionalString(params, "modelProvider"),
                                        optionalString(params, "approvalPolicy"),
                                        optionalString(params, "sandboxMode")};
                case CommandMethod::ThreadList:
                    return ThreadList{optionalString(params, "cursor"),
                                      optionalUint32(params, "limit"),
                                      optionalBool(params, "archived"),
                                      optionalString(params, "searchTerm", false)};
                case CommandMethod::ThreadRead:
                    return ThreadRead{requireString(params, "threadId"), optionalBool(params, "includeTurns")};
                case CommandMethod::TurnStart: {
                    const Json& input = requireMember(params, "input");
                    if (!input.is_array() || input.empty()) {
                        fail(ErrorCode::InvalidField, "field 'input' must be a non-empty array");
                    }
                    TurnStart start;
                    start.threadId = requireString(params, "threadId");
                    start.input.reserve(input.size());
                    for (const Json& element : input) {
                        start.input.push_back(decodeTurnInput(element));
                    }
                    start.cwd = optionalString(params, "cwd");
                    start.model = optionalString(params, "model");
                    start.reasoningEffort = optionalString(params, "reasoningEffort");
                    start.approvalPolicy = optionalString(params, "approvalPolicy");
                    const auto sandbox = params.find("sandboxPolicy");
                    if (sandbox != params.end()) {
                        start.sandboxPolicy = decodeSandboxPolicy(*sandbox);
                    }
                    return start;
                }
                case CommandMethod::TurnInterrupt:
                    return TurnInterrupt{requireString(params, "threadId"), requireString(params, "turnId")};
                case CommandMethod::ApprovalRespond: {
                    ApprovalRespond response{requireString(params, "pendingRequestId"), requireString(params, "decision")};
                    if (response.decision != "accept" && response.decision != "acceptForSession" && response.decision != "decline" &&
                        response.decision != "cancel") {
                        fail(ErrorCode::InvalidField, "field 'decision' contains an unsupported approval decision");
                    }
                    return response;
                }
                case CommandMethod::UserInputRespond: {
                    UserInputRespond response;
                    response.pendingRequestId = requireString(params, "pendingRequestId");
                    const Json& answers = requireMember(params, "answers");
                    if (!answers.is_array()) {
                        fail(ErrorCode::InvalidField, "field 'answers' must be an array");
                    }
                    response.answers.reserve(answers.size());
                    for (const Json& answer : answers) {
                        requireObject(answer, "user input answer");
                        response.answers.push_back(
                            UserInputAnswer{requireString(answer, "questionId"), requireStringArray(answer, "answers", true)});
                    }
                    return response;
                }
                case CommandMethod::AuthenticationRespond:
                    return AuthenticationRespond{requireString(params, "pendingRequestId"),
                                                 requireString(params, "accessToken"),
                                                 requireString(params, "chatgptAccountId"),
                                                 optionalString(params, "chatgptPlanType")};
                case CommandMethod::UnknownRequestRespond:
                    return UnknownRequestRespond{requireString(params, "pendingRequestId"), requireMember(params, "result")};
                case CommandMethod::UnknownRequestReject: {
                    UnknownRequestReject rejection{requireString(params, "pendingRequestId"),
                                                   requireSignedInteger(params, "code"),
                                                   requireString(params, "message"),
                                                   std::nullopt};
                    const auto data = params.find("data");
                    if (data != params.end()) {
                        rejection.data = *data;
                    }
                    return rejection;
                }
            }
            fail(ErrorCode::UnknownMethod, "unsupported frontend command");
        }

        Json parameterExtensions(CommandMethod methodValue, const Json& params) {
            switch (methodValue) {
                case CommandMethod::ControllerAcquire:
                case CommandMethod::ControllerRelease:
                case CommandMethod::SnapshotGet:
                    return extensionsOf(params, {});
                case CommandMethod::EventsReplay:
                    return extensionsOf(params, {"after"});
                case CommandMethod::ThreadStart:
                    return extensionsOf(params, {"cwd", "model", "modelProvider", "approvalPolicy", "sandboxMode", "ephemeral"});
                case CommandMethod::ThreadResume:
                    return extensionsOf(params, {"threadId", "cwd", "model", "modelProvider", "approvalPolicy", "sandboxMode"});
                case CommandMethod::ThreadList:
                    return extensionsOf(params, {"cursor", "limit", "archived", "searchTerm"});
                case CommandMethod::ThreadRead:
                    return extensionsOf(params, {"threadId", "includeTurns"});
                case CommandMethod::TurnStart:
                    return extensionsOf(params,
                                        {"threadId", "input", "cwd", "model", "reasoningEffort", "approvalPolicy", "sandboxPolicy"});
                case CommandMethod::TurnInterrupt:
                    return extensionsOf(params, {"threadId", "turnId"});
                case CommandMethod::ApprovalRespond:
                    return extensionsOf(params, {"pendingRequestId", "decision"});
                case CommandMethod::UserInputRespond:
                    return extensionsOf(params, {"pendingRequestId", "answers"});
                case CommandMethod::AuthenticationRespond:
                    return extensionsOf(params, {"pendingRequestId", "accessToken", "chatgptAccountId", "chatgptPlanType"});
                case CommandMethod::UnknownRequestRespond:
                    return extensionsOf(params, {"pendingRequestId", "result"});
                case CommandMethod::UnknownRequestReject:
                    return extensionsOf(params, {"pendingRequestId", "code", "message", "data"});
            }
            return Json::object();
        }

        Json envelope(std::string_view messageKind, const Json& extensions);

        const generated::MethodMetadata& definedMethodMetadata(generated::MethodId id) {
            const auto index = static_cast<std::size_t>(id);
            if (index >= generated::AllMethods.size()) {
                fail(ErrorCode::UnknownMethod, "unsupported frontend command");
            }
            return generated::AllMethods[index];
        }

        bool containsField(std::span<const std::string_view> fields, std::string_view field) {
            return std::find(fields.begin(), fields.end(), field) != fields.end();
        }

        void validateDefinedParameters(const generated::MethodMetadata& metadata, const Json& params) {
            validateGeneratedSchema(metadata.parameterSchema, params, "params");
        }

        generated::DefinedCommand decodeDefinedCommandImpl(const Json& message) {
            const std::string messageKind = validateEnvelope(message);
            if (messageKind != kind::Command) {
                fail(ErrorCode::UnknownKind, "expected frontend command, got '" + messageKind + "'");
            }

            generated::DefinedCommand command;
            command.requestId = requireString(message, "requestId");
            const std::string encodedMethod = requireString(message, "method");
            const auto methodId = generated::definedMethodFromString(encodedMethod);
            if (!methodId.has_value()) {
                fail(ErrorCode::UnknownMethod, "unknown frontend command method '" + encodedMethod + "'");
            }

            const generated::MethodMetadata& metadata = definedMethodMetadata(*methodId);
            const Json& params = requireMember(message, "params");
            validateDefinedParameters(metadata, params);

            Json knownParameters = Json::object();
            command.parameterExtensions = Json::object();
            for (const auto& [key, value] : params.items()) {
                if (containsField(metadata.parameterFields, key)) {
                    knownParameters[key] = value;
                } else {
                    command.parameterExtensions[key] = value;
                }
            }
            command.parameters = generated::makeParameters(*methodId, std::move(knownParameters));
            command.extensions = extensionsOf(message, {"protocol", "version", "kind", "requestId", "method", "params"});
            return command;
        }

        Json encodeDefinedCommandImpl(const generated::DefinedCommand& command) {
            if (command.requestId.empty()) {
                fail(ErrorCode::InvalidField, "command requestId must not be empty");
            }

            const generated::MethodId methodId = generated::commandMethod(command.parameters);
            const generated::MethodMetadata& metadata = definedMethodMetadata(methodId);
            Json params = std::visit(
                []<typename Parameters>(const Parameters& value) {
                    return requireObject(value.value, "params");
                },
                command.parameters);
            requireObject(command.parameterExtensions, "parameterExtensions");
            for (const auto& [key, value] : command.parameterExtensions.items()) {
                if (containsField(metadata.parameterFields, key)) {
                    fail(ErrorCode::InvalidField, "parameter extension conflicts with defined field '" + key + "'");
                }
                params[key] = value;
            }
            validateDefinedParameters(metadata, params);

            Json encoded = envelope(kind::Command, command.extensions);
            encoded["requestId"] = command.requestId;
            encoded["method"] = metadata.method;
            encoded["params"] = std::move(params);
            (void) decodeDefinedCommandImpl(encoded);
            return encoded;
        }

        ClientMessage decodeClientImpl(const Json& message) {
            const std::string messageKind = validateEnvelope(message);
            if (messageKind == kind::Hello) {
                return Hello{optionalSequence(message, "resumeAfter"),
                             extensionsOf(message, {"protocol", "version", "kind", "resumeAfter", "capabilities"}),
                             optionalCapabilities(message, "capabilities")};
            }
            if (messageKind == kind::Command) {
                Command command;
                command.requestId = requireString(message, "requestId");
                const std::string encodedMethod = requireString(message, "method");
                const auto methodValue = commandMethodFromString(encodedMethod);
                if (!methodValue.has_value()) {
                    fail(ErrorCode::UnknownMethod, "unknown frontend command method '" + encodedMethod + "'");
                }
                const Json& params = requireMember(message, "params");
                command.parameters = decodeParameters(*methodValue, params);
                command.parameterExtensions = parameterExtensions(*methodValue, params);
                command.extensions = extensionsOf(message, {"protocol", "version", "kind", "requestId", "method", "params"});
                return command;
            }
            fail(ErrorCode::UnknownKind, "unknown client message kind '" + messageKind + "'");
        }

        Json encodeTurnInput(const TurnInput& input) {
            return std::visit(
                []<typename Input>(const Input& value) {
                    using T = std::remove_cvref_t<Input>;
                    Json encoded = withExtensions(value.extensions);
                    if constexpr (std::is_same_v<T, TextInput>) {
                        encoded["type"] = "text";
                        encoded["text"] = value.text;
                    } else if constexpr (std::is_same_v<T, ImageUrlInput>) {
                        encoded["type"] = "image";
                        encoded["url"] = value.url;
                        if (value.detail.has_value()) {
                            encoded["detail"] = *value.detail;
                        }
                    } else if constexpr (std::is_same_v<T, LocalImageInput>) {
                        encoded["type"] = "localImage";
                        encoded["path"] = value.path;
                        if (value.detail.has_value()) {
                            encoded["detail"] = *value.detail;
                        }
                    } else if constexpr (std::is_same_v<T, SkillInput>) {
                        encoded["type"] = "skill";
                        encoded["name"] = value.name;
                        encoded["path"] = value.path;
                    } else {
                        encoded["type"] = "mention";
                        encoded["name"] = value.name;
                        encoded["path"] = value.path;
                    }
                    return encoded;
                },
                input);
        }

        Json encodeSandboxPolicy(const SandboxPolicy& policy) {
            Json encoded = withExtensions(policy.extensions);
            encoded["type"] = policy.type;
            if (policy.networkAccess.has_value()) {
                std::visit(
                    [&encoded](const auto& value) {
                        encoded["networkAccess"] = value;
                    },
                    *policy.networkAccess);
            }
            if (!policy.writableRoots.empty()) {
                encoded["writableRoots"] = policy.writableRoots;
            }
            if (policy.excludeTmpdirEnvVar.has_value()) {
                encoded["excludeTmpdirEnvVar"] = *policy.excludeTmpdirEnvVar;
            }
            if (policy.excludeSlashTmp.has_value()) {
                encoded["excludeSlashTmp"] = *policy.excludeSlashTmp;
            }
            // Reuse the decoder as the single source of policy validation.
            (void) decodeSandboxPolicy(encoded);
            return encoded;
        }

        template <typename Value>
        void addOptional(Json& object, std::string_view key, const std::optional<Value>& value) {
            if (value.has_value()) {
                object[std::string(key)] = *value;
            }
        }

        Json encodeParameters(const CommandParameters& parameters, const Json& extensions) {
            Json params = withExtensions(extensions);
            std::visit(
                [&params]<typename Parameters>(const Parameters& value) {
                    using T = std::remove_cvref_t<Parameters>;
                    if constexpr (std::is_same_v<T, ReplayAfter>) {
                        params["after"] = value.after.value();
                    } else if constexpr (std::is_same_v<T, ThreadStart>) {
                        addOptional(params, "cwd", value.cwd);
                        addOptional(params, "model", value.model);
                        addOptional(params, "modelProvider", value.modelProvider);
                        addOptional(params, "approvalPolicy", value.approvalPolicy);
                        addOptional(params, "sandboxMode", value.sandboxMode);
                        addOptional(params, "ephemeral", value.ephemeral);
                    } else if constexpr (std::is_same_v<T, ThreadResume>) {
                        params["threadId"] = value.threadId;
                        addOptional(params, "cwd", value.cwd);
                        addOptional(params, "model", value.model);
                        addOptional(params, "modelProvider", value.modelProvider);
                        addOptional(params, "approvalPolicy", value.approvalPolicy);
                        addOptional(params, "sandboxMode", value.sandboxMode);
                    } else if constexpr (std::is_same_v<T, ThreadList>) {
                        addOptional(params, "cursor", value.cursor);
                        addOptional(params, "limit", value.limit);
                        addOptional(params, "archived", value.archived);
                        addOptional(params, "searchTerm", value.searchTerm);
                    } else if constexpr (std::is_same_v<T, ThreadRead>) {
                        params["threadId"] = value.threadId;
                        addOptional(params, "includeTurns", value.includeTurns);
                    } else if constexpr (std::is_same_v<T, TurnStart>) {
                        params["threadId"] = value.threadId;
                        params["input"] = Json::array();
                        for (const TurnInput& input : value.input) {
                            params["input"].push_back(encodeTurnInput(input));
                        }
                        addOptional(params, "cwd", value.cwd);
                        addOptional(params, "model", value.model);
                        addOptional(params, "reasoningEffort", value.reasoningEffort);
                        addOptional(params, "approvalPolicy", value.approvalPolicy);
                        if (value.sandboxPolicy.has_value()) {
                            params["sandboxPolicy"] = encodeSandboxPolicy(*value.sandboxPolicy);
                        }
                    } else if constexpr (std::is_same_v<T, TurnInterrupt>) {
                        params["threadId"] = value.threadId;
                        params["turnId"] = value.turnId;
                    } else if constexpr (std::is_same_v<T, ApprovalRespond>) {
                        params["pendingRequestId"] = value.pendingRequestId;
                        params["decision"] = value.decision;
                    } else if constexpr (std::is_same_v<T, UserInputRespond>) {
                        params["pendingRequestId"] = value.pendingRequestId;
                        params["answers"] = Json::array();
                        for (const UserInputAnswer& answer : value.answers) {
                            params["answers"].push_back({{"questionId", answer.questionId}, {"answers", answer.answers}});
                        }
                    } else if constexpr (std::is_same_v<T, AuthenticationRespond>) {
                        params["pendingRequestId"] = value.pendingRequestId;
                        params["accessToken"] = value.accessToken;
                        params["chatgptAccountId"] = value.chatgptAccountId;
                        addOptional(params, "chatgptPlanType", value.chatgptPlanType);
                    } else if constexpr (std::is_same_v<T, UnknownRequestRespond>) {
                        params["pendingRequestId"] = value.pendingRequestId;
                        params["result"] = value.result;
                    } else if constexpr (std::is_same_v<T, UnknownRequestReject>) {
                        params["pendingRequestId"] = value.pendingRequestId;
                        params["code"] = value.code;
                        params["message"] = value.message;
                        addOptional(params, "data", value.data);
                    }
                },
                parameters);
            return params;
        }

        Json envelope(std::string_view messageKind, const Json& extensions) {
            Json encoded = withExtensions(extensions);
            encoded["protocol"] = ProtocolIdentity;
            encoded["version"] = ProtocolVersion;
            encoded["kind"] = messageKind;
            return encoded;
        }

        Json encodeClientImpl(const ClientMessage& message) {
            Json encoded = std::visit(
                []<typename Message>(const Message& value) {
                    using T = std::remove_cvref_t<Message>;
                    if constexpr (std::is_same_v<T, Hello>) {
                        Json result = envelope(kind::Hello, value.extensions);
                        if (value.resumeAfter.has_value()) {
                            result["resumeAfter"] = value.resumeAfter->value();
                        }
                        if (value.capabilities.has_value()) {
                            result["capabilities"] = encodeCapabilities(*value.capabilities);
                        }
                        return result;
                    } else {
                        if (value.requestId.empty()) {
                            fail(ErrorCode::InvalidField, "command requestId must not be empty");
                        }
                        Json result = envelope(kind::Command, value.extensions);
                        result["requestId"] = value.requestId;
                        result["method"] = toString(commandMethod(value.parameters));
                        result["params"] = encodeParameters(value.parameters, value.parameterExtensions);
                        // Validate all command-specific invariants before handing
                        // the encoded object to a transport.
                        (void) decodeClientImpl(result);
                        return result;
                    }
                },
                message);
            return encoded;
        }

        FrontendEvent decodeEventImpl(const Json& encoded) {
            requireObject(encoded, "event");
            FrontendEvent event;
            event.sequence = requireSequence(encoded, "sequence");
            if (event.sequence.value() == 0) {
                fail(ErrorCode::InvalidField, "event field 'sequence' must be greater than zero");
            }
            event.type = requireString(encoded, "type");
            event.data = requireMember(encoded, "data");
            if (!event.data.is_object()) {
                fail(ErrorCode::InvalidField, "event field 'data' must be an object");
            }
            event.extensions = extensionsOf(encoded, {"sequence", "type", "data"});
            return event;
        }

        Json encodeEventImpl(const FrontendEvent& event) {
            if (event.sequence.value() == 0) {
                fail(ErrorCode::InvalidField, "event sequence must be greater than zero");
            }
            if (event.type.empty()) {
                fail(ErrorCode::InvalidField, "event type must not be empty");
            }
            if (!event.data.is_object()) {
                fail(ErrorCode::InvalidField, "event data must be an object");
            }
            Json encoded = withExtensions(event.extensions);
            encoded["sequence"] = event.sequence.value();
            encoded["type"] = event.type;
            encoded["data"] = event.data;
            return encoded;
        }

        ExpandedThreadItem decodeExpandedThreadItem(const Json& encoded) {
            requireObject(encoded, "expanded thread item");
            const std::string kindName = requireString(encoded, "type");
            const auto kindValue = threadItemKindFromString(kindName);
            if (!kindValue.has_value()) {
                fail(ErrorCode::InvalidField, "unknown expanded thread item kind '" + kindName + "'");
            }
            ExpandedThreadItem item;
            item.id = requireString(encoded, "id");
            item.type = *kindValue;
            item.threadId = optionalString(encoded, "threadId");
            item.turnId = optionalString(encoded, "turnId");
            item.status = optionalString(encoded, "status", false);
            item.summary = optionalString(encoded, "summary", false);
            if (const auto location = encoded.find("location"); location != encoded.end()) {
                item.location = requireObject(*location, "location");
            }
            item.agentText = optionalString(encoded, "agentText", false);
            item.reasoningText = optionalString(encoded, "reasoningText", false);
            item.reasoningSummary = optionalString(encoded, "reasoningSummary", false);
            item.commandOutput = optionalString(encoded, "commandOutput", false);
            if (const auto dropped = encoded.find("droppedContentBytes"); dropped != encoded.end()) {
                item.droppedContentBytes = unsignedInteger(*dropped, "droppedContentBytes");
            }
            item.contentTruncated = optionalBool(encoded, "contentTruncated");
            if (encoded.contains("startedAtMs")) {
                item.startedAtMs = requireSignedInteger(encoded, "startedAtMs");
            }
            if (encoded.contains("completedAtMs")) {
                item.completedAtMs = requireSignedInteger(encoded, "completedAtMs");
            }
            if (const auto data = encoded.find("data"); data != encoded.end()) {
                item.data = requireObject(*data, "data");
            }
            item.truncated = optionalBool(encoded, "truncated").value_or(false);
            item.omittedFields = optionalStringArray(encoded, "omittedFields").value_or(std::vector<std::string>{});
            item.connectionInvalidated = optionalBool(encoded, "connectionInvalidated").value_or(false);
            if (const auto generation = encoded.find("generation"); generation != encoded.end()) {
                item.generation = unsignedInteger(*generation, "generation");
            }
            if (const auto freshness = optionalString(encoded, "freshness"); freshness.has_value()) {
                item.freshness = stateFreshnessFromString(*freshness);
                if (!item.freshness.has_value()) {
                    fail(ErrorCode::InvalidField, "unknown expanded item freshness '" + *freshness + "'");
                }
            }
            item.extensions = extensionsOf(encoded,
                                           {"id",
                                            "type",
                                            "threadId",
                                            "turnId",
                                            "status",
                                            "summary",
                                            "location",
                                            "agentText",
                                            "reasoningText",
                                            "reasoningSummary",
                                            "commandOutput",
                                            "droppedContentBytes",
                                            "contentTruncated",
                                            "startedAtMs",
                                            "completedAtMs",
                                            "data",
                                            "truncated",
                                            "omittedFields",
                                            "connectionInvalidated",
                                            "generation",
                                            "freshness"});
            return item;
        }

        Json encodeExpandedThreadItem(const ExpandedThreadItem& item) {
            if (item.id.empty()) {
                fail(ErrorCode::InvalidField, "expanded thread item id must not be empty");
            }
            Json encoded = withExtensions(item.extensions);
            encoded["id"] = item.id;
            encoded["type"] = toString(item.type);
            addOptional(encoded, "threadId", item.threadId);
            addOptional(encoded, "turnId", item.turnId);
            addOptional(encoded, "status", item.status);
            addOptional(encoded, "summary", item.summary);
            if (item.location.has_value()) {
                encoded["location"] = requireObject(*item.location, "location");
            }
            addOptional(encoded, "agentText", item.agentText);
            addOptional(encoded, "reasoningText", item.reasoningText);
            addOptional(encoded, "reasoningSummary", item.reasoningSummary);
            addOptional(encoded, "commandOutput", item.commandOutput);
            addOptional(encoded, "droppedContentBytes", item.droppedContentBytes);
            addOptional(encoded, "contentTruncated", item.contentTruncated);
            addOptional(encoded, "startedAtMs", item.startedAtMs);
            addOptional(encoded, "completedAtMs", item.completedAtMs);
            if (item.data.has_value()) {
                encoded["data"] = requireObject(*item.data, "data");
            }
            encoded["truncated"] = item.truncated;
            if (!item.omittedFields.empty()) {
                encoded["omittedFields"] = item.omittedFields;
            }
            encoded["connectionInvalidated"] = item.connectionInvalidated;
            addOptional(encoded, "generation", item.generation);
            if (item.freshness.has_value()) {
                encoded["freshness"] = toString(*item.freshness);
            }
            return encoded;
        }

        ExpandedPendingRequest decodeExpandedPendingRequest(const Json& encoded) {
            requireObject(encoded, "expanded pending request");
            const std::string kindName = requireString(encoded, "kind");
            const auto kindValue = pendingRequestKindFromString(kindName);
            if (!kindValue.has_value()) {
                fail(ErrorCode::InvalidField, "unknown expanded pending-request kind '" + kindName + "'");
            }
            ExpandedPendingRequest request;
            request.pendingRequestId = requireString(encoded, "pendingRequestId");
            request.kind = *kindValue;
            request.threadId = optionalString(encoded, "threadId");
            request.turnId = optionalString(encoded, "turnId");
            request.itemId = optionalString(encoded, "itemId");
            request.summary = optionalString(encoded, "summary", false);
            if (const auto details = encoded.find("details"); details != encoded.end()) {
                request.details = requireObject(*details, "details");
            }
            request.truncated = optionalBool(encoded, "truncated").value_or(false);
            request.extensions =
                extensionsOf(encoded, {"pendingRequestId", "kind", "threadId", "turnId", "itemId", "summary", "details", "truncated"});
            return request;
        }

        Json encodeExpandedPendingRequest(const ExpandedPendingRequest& request) {
            if (request.pendingRequestId.empty()) {
                fail(ErrorCode::InvalidField, "expanded pendingRequestId must not be empty");
            }
            Json encoded = withExtensions(request.extensions);
            encoded["pendingRequestId"] = request.pendingRequestId;
            encoded["kind"] = toString(request.kind);
            addOptional(encoded, "threadId", request.threadId);
            addOptional(encoded, "turnId", request.turnId);
            addOptional(encoded, "itemId", request.itemId);
            addOptional(encoded, "summary", request.summary);
            if (request.details.has_value()) {
                encoded["details"] = requireObject(*request.details, "details");
            }
            encoded["truncated"] = request.truncated;
            return encoded;
        }

        std::vector<Json> requireObjectArray(const Json& object, std::string_view field) {
            const Json& encoded = requireMember(object, field);
            if (!encoded.is_array()) {
                fail(ErrorCode::InvalidField, "field '" + std::string(field) + "' must be an array");
            }
            std::vector<Json> values;
            values.reserve(encoded.size());
            for (const Json& value : encoded) {
                values.push_back(requireObject(value, field));
            }
            return values;
        }

        std::optional<Json> optionalObject(const Json& object, std::string_view field) {
            const auto member = object.find(std::string(field));
            if (member == object.end()) {
                return std::nullopt;
            }
            return std::optional<Json>{Json(requireObject(*member, field))};
        }

        ExpandedBackendSnapshotState decodeExpandedState(const Json& encoded) {
            requireObject(encoded, "expanded snapshot state");
            ExpandedBackendSnapshotState state;
            state.provider = requireObject(requireMember(encoded, "provider"), "provider");
            state.controller = requireObject(requireMember(encoded, "controller"), "controller");
            state.sessions = requireObjectArray(encoded, "sessions");
            state.capacity = requireObject(requireMember(encoded, "capacity"), "capacity");
            state.truncation = requireObject(requireMember(encoded, "truncation"), "truncation");

            if (encoded.contains("threads")) {
                state.threads = requireObjectArray(encoded, "threads");
            }
            if (encoded.contains("turns")) {
                state.turns = requireObjectArray(encoded, "turns");
            }
            if (const auto items = encoded.find("items"); items != encoded.end()) {
                if (!items->is_array()) {
                    fail(ErrorCode::InvalidField, "field 'items' must be an array");
                }
                state.items.emplace();
                state.items->reserve(items->size());
                for (const Json& item : *items) {
                    state.items->push_back(decodeExpandedThreadItem(item));
                }
            }
            if (const auto requests = encoded.find("pendingRequests"); requests != encoded.end()) {
                if (!requests->is_array()) {
                    fail(ErrorCode::InvalidField, "field 'pendingRequests' must be an array");
                }
                state.pendingRequests.emplace();
                state.pendingRequests->reserve(requests->size());
                for (const Json& request : *requests) {
                    state.pendingRequests->push_back(decodeExpandedPendingRequest(request));
                }
            }

            state.accounts = optionalObject(encoded, "accounts");
            state.models = optionalObject(encoded, "models");
            state.configuration = optionalObject(encoded, "configuration");
            state.processes = optionalObject(encoded, "processes");
            state.filesystemWatches = optionalObject(encoded, "filesystemWatches");
            state.fuzzySearches = optionalObject(encoded, "fuzzySearches");
            state.permissionProfiles = optionalObject(encoded, "permissionProfiles");
            state.reviews = optionalObject(encoded, "reviews");
            state.apps = optionalObject(encoded, "apps");
            state.externalAgents = optionalObject(encoded, "externalAgents");
            state.hooks = optionalObject(encoded, "hooks");
            state.marketplace = optionalObject(encoded, "marketplace");
            state.plugins = optionalObject(encoded, "plugins");
            state.skills = optionalObject(encoded, "skills");
            state.mcp = optionalObject(encoded, "mcp");
            state.windowsSandbox = optionalObject(encoded, "windowsSandbox");
            state.remoteControl = optionalObject(encoded, "remoteControl");
            state.notices = optionalObject(encoded, "notices");
            state.activities = optionalObject(encoded, "activities");
            state.extensions = extensionsOf(encoded, {"provider",        "controller",
                                                      "sessions",        "threads",
                                                      "turns",           "items",
                                                      "pendingRequests", "accounts",
                                                      "models",          "configuration",
                                                      "processes",       "filesystemWatches",
                                                      "fuzzySearches",   "permissionProfiles",
                                                      "reviews",         "apps",
                                                      "externalAgents",  "hooks",
                                                      "marketplace",     "plugins",
                                                      "skills",          "mcp",
                                                      "windowsSandbox",  "remoteControl",
                                                      "notices",         "activities",
                                                      "capacity",        "truncation"});
            return state;
        }

        Json encodeExpandedState(const ExpandedBackendSnapshotState& state) {
            Json encoded = withExtensions(state.extensions);
            encoded["provider"] = requireObject(state.provider, "provider");
            encoded["controller"] = requireObject(state.controller, "controller");
            encoded["sessions"] = state.sessions;
            for (const Json& session : state.sessions) {
                (void) requireObject(session, "sessions");
            }
            encoded["capacity"] = requireObject(state.capacity, "capacity");
            encoded["truncation"] = requireObject(state.truncation, "truncation");
            addOptional(encoded, "threads", state.threads);
            addOptional(encoded, "turns", state.turns);
            if (state.items.has_value()) {
                encoded["items"] = Json::array();
                for (const ExpandedThreadItem& item : *state.items) {
                    encoded["items"].push_back(encodeExpandedThreadItem(item));
                }
            }
            if (state.pendingRequests.has_value()) {
                encoded["pendingRequests"] = Json::array();
                for (const ExpandedPendingRequest& request : *state.pendingRequests) {
                    encoded["pendingRequests"].push_back(encodeExpandedPendingRequest(request));
                }
            }
            addOptional(encoded, "accounts", state.accounts);
            addOptional(encoded, "models", state.models);
            addOptional(encoded, "configuration", state.configuration);
            addOptional(encoded, "processes", state.processes);
            addOptional(encoded, "filesystemWatches", state.filesystemWatches);
            addOptional(encoded, "fuzzySearches", state.fuzzySearches);
            addOptional(encoded, "permissionProfiles", state.permissionProfiles);
            addOptional(encoded, "reviews", state.reviews);
            addOptional(encoded, "apps", state.apps);
            addOptional(encoded, "externalAgents", state.externalAgents);
            addOptional(encoded, "hooks", state.hooks);
            addOptional(encoded, "marketplace", state.marketplace);
            addOptional(encoded, "plugins", state.plugins);
            addOptional(encoded, "skills", state.skills);
            addOptional(encoded, "mcp", state.mcp);
            addOptional(encoded, "windowsSandbox", state.windowsSandbox);
            addOptional(encoded, "remoteControl", state.remoteControl);
            addOptional(encoded, "notices", state.notices);
            addOptional(encoded, "activities", state.activities);
            return encoded;
        }

        ExpandedSnapshot decodeExpandedSnapshotImpl(const Json& message) {
            const std::string messageKind = validateEnvelope(message);
            if (messageKind != kind::Snapshot) {
                fail(ErrorCode::UnknownKind, "expected expanded snapshot, got '" + messageKind + "'");
            }
            validateGeneratedSchema("#/$defs/ExpandedSnapshot", message, "expanded snapshot");
            return ExpandedSnapshot{
                requireSequence(message, "sequence"),
                decodeExpandedState(requireMember(message, "state")),
                extensionsOf(message, {"protocol", "version", "kind", "sequence", "state"}),
            };
        }

        Json encodeExpandedSnapshotImpl(const ExpandedSnapshot& snapshot) {
            Json encoded = envelope(kind::Snapshot, snapshot.extensions);
            encoded["sequence"] = snapshot.sequence.value();
            encoded["state"] = encodeExpandedState(snapshot.state);
            validateGeneratedSchema("#/$defs/ExpandedSnapshot", encoded, "expanded snapshot");
            return encoded;
        }

        ExpandedFrontendEvent decodeExpandedEventImpl(const Json& encoded) {
            requireObject(encoded, "expanded event");
            validateGeneratedSchema("#/$defs/ExpandedFrontendEvent", encoded, "expanded event");
            const std::string typeName = requireString(encoded, "type");
            const auto typeValue = expandedEventTypeFromString(typeName);
            if (!typeValue.has_value()) {
                fail(ErrorCode::InvalidField, "unknown expanded event type '" + typeName + "'");
            }
            const Json& data = requireObject(requireMember(encoded, "data"), "data");
            return ExpandedFrontendEvent{
                requireSequence(encoded, "sequence"), *typeValue, data, extensionsOf(encoded, {"sequence", "type", "data"})};
        }

        Json encodeExpandedEventImpl(const ExpandedFrontendEvent& event) {
            if (event.sequence.value() == 0) {
                fail(ErrorCode::InvalidField, "expanded event sequence must be greater than zero");
            }
            Json encoded = withExtensions(event.extensions);
            encoded["sequence"] = event.sequence.value();
            encoded["type"] = toString(event.type);
            encoded["data"] = requireObject(event.data, "data");
            validateGeneratedSchema("#/$defs/ExpandedFrontendEvent", encoded, "expanded event");
            return encoded;
        }

        CommandError decodeCommandError(const Json& encoded) {
            requireObject(encoded, "error");
            const std::string codeString = requireString(encoded, "code");
            const auto code = errorCodeFromString(codeString);
            if (!code.has_value()) {
                fail(ErrorCode::InvalidField, "unknown frontend error code '" + codeString + "'");
            }
            CommandError error{
                *code, requireString(encoded, "message", false), std::nullopt, extensionsOf(encoded, {"code", "message", "details"})};
            const auto details = encoded.find("details");
            if (details != encoded.end()) {
                error.details = *details;
            }
            return error;
        }

        Json encodeCommandError(const CommandError& error) {
            Json encoded = withExtensions(error.extensions);
            encoded["code"] = toString(error.code);
            encoded["message"] = error.message;
            if (error.details.has_value()) {
                encoded["details"] = *error.details;
            }
            return encoded;
        }

        std::vector<std::uint32_t> decodeSupportedVersions(const Json& object) {
            const Json& encoded = requireMember(object, "supportedVersions");
            if (!encoded.is_array()) {
                fail(ErrorCode::InvalidField, "field 'supportedVersions' must be an array");
            }
            std::vector<std::uint32_t> versions;
            versions.reserve(encoded.size());
            for (const Json& entry : encoded) {
                const std::uint64_t version = unsignedInteger(entry, "supportedVersions");
                if (version > std::numeric_limits<std::uint32_t>::max()) {
                    fail(ErrorCode::InvalidField, "supported protocol version exceeds the unsigned 32-bit range");
                }
                versions.push_back(static_cast<std::uint32_t>(version));
            }
            return versions;
        }

        ServerMessage decodeServerImpl(const Json& message) {
            const std::string messageKind = validateEnvelope(message);
            if (messageKind == kind::Welcome) {
                const std::string encodedRole = requireString(message, "role");
                const auto role = sessionRoleFromString(encodedRole);
                if (!role.has_value()) {
                    fail(ErrorCode::InvalidField, "unknown session role '" + encodedRole + "'");
                }
                const std::string encodedSyncMode = requireString(message, "syncMode");
                const auto syncMode = syncModeFromString(encodedSyncMode);
                if (!syncMode.has_value()) {
                    fail(ErrorCode::InvalidField, "unknown synchronization mode '" + encodedSyncMode + "'");
                }
                Welcome welcome{requireString(message, "sessionId"),
                                *role,
                                requireSequence(message, "currentSequence"),
                                *syncMode,
                                extensionsOf(message,
                                             {"protocol",
                                              "version",
                                              "kind",
                                              "sessionId",
                                              "role",
                                              "currentSequence",
                                              "syncMode",
                                              "capabilities",
                                              "availableMethods",
                                              "permittedMethods",
                                              "serverVersion"})};
                const auto capabilities = message.find("capabilities");
                if (capabilities != message.end()) {
                    welcome.capabilities = decodeCapabilityAdvertisement(*capabilities);
                }
                welcome.availableMethods = optionalMethods(message, "availableMethods");
                welcome.permittedMethods = optionalMethods(message, "permittedMethods");
                welcome.serverVersion = optionalString(message, "serverVersion");
                return welcome;
            }
            if (messageKind == kind::SyncComplete) {
                return SyncComplete{requireSequence(message, "sequence"),
                                    extensionsOf(message, {"protocol", "version", "kind", "sequence"})};
            }
            if (messageKind == kind::Snapshot) {
                Json state = requireMember(message, "state");
                if (!state.is_object()) {
                    fail(ErrorCode::InvalidField, "snapshot field 'state' must be an object");
                }
                return Snapshot{requireSequence(message, "sequence"),
                                std::move(state),
                                extensionsOf(message, {"protocol", "version", "kind", "sequence", "state"})};
            }
            if (messageKind == kind::Events) {
                EventBatch batch;
                batch.fromSequence = requireSequence(message, "fromSequence");
                batch.toSequence = requireSequence(message, "toSequence");
                const Json& events = requireMember(message, "events");
                if (!events.is_array() || events.empty()) {
                    fail(ErrorCode::InvalidField, "event batch field 'events' must be a non-empty array");
                }
                batch.events.reserve(events.size());
                for (const Json& event : events) {
                    FrontendEvent decoded = decodeEventImpl(event);
                    if (!batch.events.empty() && decoded.sequence <= batch.events.back().sequence) {
                        fail(ErrorCode::InvalidField, "event batch sequence numbers must be strictly increasing");
                    }
                    batch.events.push_back(std::move(decoded));
                }
                if (batch.fromSequence != batch.events.front().sequence || batch.toSequence != batch.events.back().sequence) {
                    fail(ErrorCode::InvalidField, "event batch range does not match its first and last events");
                }
                batch.extensions = extensionsOf(message, {"protocol", "version", "kind", "fromSequence", "toSequence", "events"});
                return batch;
            }
            if (messageKind == kind::Response) {
                Response response;
                response.requestId = requireString(message, "requestId");
                response.ok = requireBool(message, "ok");
                const auto result = message.find("result");
                const auto error = message.find("error");
                if (response.ok) {
                    if (result == message.end() || error != message.end()) {
                        fail(ErrorCode::InvalidField, "successful response must contain result and must not contain error");
                    }
                    response.result = *result;
                } else {
                    if (error == message.end() || result != message.end()) {
                        fail(ErrorCode::InvalidField, "failed response must contain error and must not contain result");
                    }
                    response.error = decodeCommandError(*error);
                }
                response.extensions = extensionsOf(message, {"protocol", "version", "kind", "requestId", "ok", "result", "error"});
                return response;
            }
            if (messageKind == kind::ProtocolError) {
                const std::string codeString = requireString(message, "code");
                const auto code = errorCodeFromString(codeString);
                if (!code.has_value()) {
                    fail(ErrorCode::InvalidField, "unknown frontend error code '" + codeString + "'");
                }
                ProtocolErrorMessage protocolError;
                protocolError.code = *code;
                protocolError.message = requireString(message, "message", false);
                protocolError.supportedVersions = decodeSupportedVersions(message);
                protocolError.closeConnection = requireBool(message, "closeConnection");
                protocolError.requestId = optionalString(message, "requestId");
                const auto details = message.find("details");
                if (details != message.end()) {
                    protocolError.details = *details;
                }
                protocolError.extensions = extensionsOf(
                    message,
                    {"protocol", "version", "kind", "code", "message", "supportedVersions", "closeConnection", "requestId", "details"});
                return protocolError;
            }
            fail(ErrorCode::UnknownKind, "unknown server message kind '" + messageKind + "'");
        }

        Json encodeServerImpl(const ServerMessage& message) {
            return std::visit(
                []<typename Message>(const Message& value) {
                    using T = std::remove_cvref_t<Message>;
                    if constexpr (std::is_same_v<T, Welcome>) {
                        if (value.sessionId.empty()) {
                            fail(ErrorCode::InvalidField, "welcome sessionId must not be empty");
                        }
                        Json encoded = envelope(kind::Welcome, value.extensions);
                        encoded["sessionId"] = value.sessionId;
                        encoded["role"] = toString(value.role);
                        encoded["currentSequence"] = value.currentSequence.value();
                        encoded["syncMode"] = toString(value.syncMode);
                        if (value.capabilities.has_value()) {
                            encoded["capabilities"] = encodeCapabilityAdvertisement(*value.capabilities);
                        }
                        if (value.availableMethods.has_value()) {
                            encoded["availableMethods"] = encodeMethods(*value.availableMethods);
                        }
                        if (value.permittedMethods.has_value()) {
                            encoded["permittedMethods"] = encodeMethods(*value.permittedMethods);
                        }
                        addOptional(encoded, "serverVersion", value.serverVersion);
                        return encoded;
                    } else if constexpr (std::is_same_v<T, SyncComplete>) {
                        Json encoded = envelope(kind::SyncComplete, value.extensions);
                        encoded["sequence"] = value.sequence.value();
                        return encoded;
                    } else if constexpr (std::is_same_v<T, Snapshot>) {
                        if (!value.state.is_object()) {
                            fail(ErrorCode::InvalidField, "snapshot state must be an object");
                        }
                        Json encoded = envelope(kind::Snapshot, value.extensions);
                        encoded["sequence"] = value.sequence.value();
                        encoded["state"] = value.state;
                        return encoded;
                    } else if constexpr (std::is_same_v<T, EventBatch>) {
                        if (value.events.empty()) {
                            fail(ErrorCode::InvalidField, "event batch must not be empty");
                        }
                        Json encoded = envelope(kind::Events, value.extensions);
                        encoded["fromSequence"] = value.fromSequence.value();
                        encoded["toSequence"] = value.toSequence.value();
                        encoded["events"] = Json::array();
                        SequenceNumber previous;
                        bool first = true;
                        for (const FrontendEvent& event : value.events) {
                            if (!first && event.sequence <= previous) {
                                fail(ErrorCode::InvalidField, "event batch sequence numbers must be strictly increasing");
                            }
                            encoded["events"].push_back(encodeEventImpl(event));
                            previous = event.sequence;
                            first = false;
                        }
                        if (value.fromSequence != value.events.front().sequence || value.toSequence != value.events.back().sequence) {
                            fail(ErrorCode::InvalidField, "event batch range does not match its first and last events");
                        }
                        return encoded;
                    } else if constexpr (std::is_same_v<T, Response>) {
                        if (value.requestId.empty()) {
                            fail(ErrorCode::InvalidField, "response requestId must not be empty");
                        }
                        Json encoded = envelope(kind::Response, value.extensions);
                        encoded["requestId"] = value.requestId;
                        encoded["ok"] = value.ok;
                        if (value.ok) {
                            if (!value.result.has_value() || value.error.has_value()) {
                                fail(ErrorCode::InvalidField, "successful response must contain exactly one result");
                            }
                            encoded["result"] = *value.result;
                        } else {
                            if (!value.error.has_value() || value.result.has_value()) {
                                fail(ErrorCode::InvalidField, "failed response must contain exactly one error");
                            }
                            encoded["error"] = encodeCommandError(*value.error);
                        }
                        return encoded;
                    } else {
                        Json encoded = envelope(kind::ProtocolError, value.extensions);
                        encoded["code"] = toString(value.code);
                        encoded["message"] = value.message;
                        encoded["supportedVersions"] = value.supportedVersions;
                        encoded["closeConnection"] = value.closeConnection;
                        if (value.requestId.has_value()) {
                            encoded["requestId"] = *value.requestId;
                        }
                        if (value.details.has_value()) {
                            encoded["details"] = *value.details;
                        }
                        return encoded;
                    }
                },
                message);
        }

        template <typename Message>
        CodecResult<Message> parseAndDecode(std::string_view encoded, CodecResult<Message> (*decoder)(const Json&) noexcept) noexcept {
            try {
                Json parsed = Json::parse(encoded.begin(), encoded.end(), nullptr, false);
                if (parsed.is_discarded()) {
                    return CodecError{ErrorCode::MalformedJson, "message is not valid JSON", false, {}, std::nullopt, std::nullopt};
                }
                return decoder(parsed);
            } catch (const std::exception& exception) {
                return CodecError{ErrorCode::InternalError,
                                  std::string("failed to parse frontend protocol JSON: ") + exception.what(),
                                  false,
                                  {},
                                  std::nullopt,
                                  std::nullopt};
            } catch (...) {
                return CodecError{ErrorCode::InternalError,
                                  "failed to parse frontend protocol JSON: unknown local exception",
                                  false,
                                  {},
                                  std::nullopt,
                                  std::nullopt};
            }
        }

    } // namespace

    CodecResult<ClientMessage> Codec::decodeClient(const Json& message) noexcept {
        return guard<ClientMessage>(
            [&message]() {
                return decodeClientImpl(message);
            },
            &message);
    }

    CodecResult<ClientMessage> Codec::decodeClient(std::string_view message) noexcept {
        return parseAndDecode<ClientMessage>(message,
                                             static_cast<CodecResult<ClientMessage> (*)(const Json&) noexcept>(&Codec::decodeClient));
    }

    CodecResult<generated::DefinedCommand> Codec::decodeDefinedCommand(const Json& message) noexcept {
        return guard<generated::DefinedCommand>(
            [&message]() {
                return decodeDefinedCommandImpl(message);
            },
            &message);
    }

    CodecResult<generated::DefinedCommand> Codec::decodeDefinedCommand(std::string_view message) noexcept {
        return parseAndDecode<generated::DefinedCommand>(
            message, static_cast<CodecResult<generated::DefinedCommand> (*)(const Json&) noexcept>(&Codec::decodeDefinedCommand));
    }

    CodecResult<ServerMessage> Codec::decodeServer(const Json& message) noexcept {
        return guard<ServerMessage>(
            [&message]() {
                return decodeServerImpl(message);
            },
            &message);
    }

    CodecResult<ServerMessage> Codec::decodeServer(std::string_view message) noexcept {
        return parseAndDecode<ServerMessage>(message,
                                             static_cast<CodecResult<ServerMessage> (*)(const Json&) noexcept>(&Codec::decodeServer));
    }

    CodecResult<Json> Codec::encodeClient(const ClientMessage& message) noexcept {
        return guard<Json>([&message]() {
            return encodeClientImpl(message);
        });
    }

    CodecResult<Json> Codec::encodeDefinedCommand(const generated::DefinedCommand& command) noexcept {
        return guard<Json>([&command]() {
            return encodeDefinedCommandImpl(command);
        });
    }

    CodecResult<generated::CompleteCommandResult> Codec::decodeDefinedResult(generated::MethodId method, const Json& result) noexcept {
        return guard<generated::CompleteCommandResult>([method, &result]() {
            const generated::MethodMetadata& metadata = definedMethodMetadata(method);
            validateGeneratedSchema(metadata.resultSchema, result, "result");
            return generated::makeResult(method, result);
        });
    }

    CodecResult<Json> Codec::encodeDefinedResult(const generated::CompleteCommandResult& result) noexcept {
        return guard<Json>([&result]() {
            const generated::MethodMetadata& metadata = definedMethodMetadata(generated::commandMethod(result));
            return std::visit(
                [&metadata]<typename Result>(const Result& value) {
                    validateGeneratedSchema(metadata.resultSchema, value.value, "result");
                    return value.value;
                },
                result);
        });
    }

    CodecResult<Json> Codec::encodeServer(const ServerMessage& message) noexcept {
        return guard<Json>([&message]() {
            return encodeServerImpl(message);
        });
    }

    CodecResult<std::string> Codec::serializeClient(const ClientMessage& message) noexcept {
        const auto encoded = encodeClient(message);
        if (!encoded) {
            return encoded.error();
        }
        return guard<std::string>([&encoded]() {
            return encoded.value().dump();
        });
    }

    CodecResult<std::string> Codec::serializeDefinedCommand(const generated::DefinedCommand& command) noexcept {
        const auto encoded = encodeDefinedCommand(command);
        if (!encoded) {
            return encoded.error();
        }
        return guard<std::string>([&encoded]() {
            return encoded.value().dump();
        });
    }

    CodecResult<std::string> Codec::serializeServer(const ServerMessage& message) noexcept {
        const auto encoded = encodeServer(message);
        if (!encoded) {
            return encoded.error();
        }
        return guard<std::string>([&encoded]() {
            return encoded.value().dump();
        });
    }

    CodecResult<ExpandedSnapshot> Codec::decodeExpandedSnapshot(const Json& message) noexcept {
        return guard<ExpandedSnapshot>(
            [&message]() {
                return decodeExpandedSnapshotImpl(message);
            },
            &message);
    }

    CodecResult<Json> Codec::encodeExpandedSnapshot(const ExpandedSnapshot& snapshot) noexcept {
        return guard<Json>([&snapshot]() {
            return encodeExpandedSnapshotImpl(snapshot);
        });
    }

    CodecResult<ExpandedFrontendEvent> Codec::decodeExpandedEvent(const Json& event) noexcept {
        return guard<ExpandedFrontendEvent>([&event]() {
            return decodeExpandedEventImpl(event);
        });
    }

    CodecResult<Json> Codec::encodeExpandedEvent(const ExpandedFrontendEvent& event) noexcept {
        return guard<Json>([&event]() {
            return encodeExpandedEventImpl(event);
        });
    }

    CodecResult<Json> Codec::encodeEvent(const FrontendEvent& event) noexcept {
        return guard<Json>([&event]() {
            return encodeEventImpl(event);
        });
    }

    CodecResult<std::size_t> Codec::serializedEventSize(const FrontendEvent& event) noexcept {
        const auto encoded = encodeEvent(event);
        if (!encoded) {
            return encoded.error();
        }
        return guard<std::size_t>([&encoded]() {
            return encoded.value().dump().size();
        });
    }

} // namespace ai::openai::codex::frontend
