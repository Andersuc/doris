// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <fmt/format.h>
#include <glog/logging.h>
#include <string.h>

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "common/status.h"
#include "runtime/primitive_type.h"
#include "vec/columns/column.h"
#include "vec/columns/column_array.h"
#include "vec/columns/column_decimal.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_string.h"
#include "vec/columns/column_vector.h"
#include "vec/common/assert_cast.h"
#include "vec/common/pod_array_fwd.h"
#include "vec/core/block.h"
#include "vec/core/column_numbers.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/columns_with_type_and_name.h"
#include "vec/core/types.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_array.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/functions/function.h"

namespace doris {
class FunctionContext;
} // namespace doris

namespace doris::vectorized {

class FunctionArrayRemove : public IFunction {
public:
    static constexpr auto name = "array_remove";
    static FunctionPtr create() { return std::make_shared<FunctionArrayRemove>(); }

    /// Get function name.
    String get_name() const override { return name; }

    bool is_variadic() const override { return false; }

    size_t get_number_of_arguments() const override { return 2; }

    DataTypePtr get_return_type_impl(const DataTypes& arguments) const override {
        DCHECK(arguments[0]->get_primitive_type() == TYPE_ARRAY)
                << "First argument for function: " << name
                << " should be DataTypeArray but it has type " << arguments[0]->get_name() << ".";
        return arguments[0];
    }

    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const override {
        // For default implementation of nulls args
        ColumnsWithTypeAndName args = {block.get_by_position(arguments[0]),
                                       block.get_by_position(arguments[1])};

        auto res_column = _execute_non_nullable(args, input_rows_count);
        if (!res_column) {
            return Status::RuntimeError(
                    fmt::format("unsupported types for function {}({}, {})", get_name(),
                                block.get_by_position(arguments[0]).type->get_name(),
                                block.get_by_position(arguments[1]).type->get_name()));
        }
        DCHECK_EQ(args[0].column->size(), res_column->size());
        block.replace_by_position(result, std::move(res_column));
        return Status::OK();
    }

private:
    template <typename NestedColumnType, typename RightColumnType>
    ColumnPtr _execute_number(const ColumnArray::Offsets64& offsets, const IColumn& nested_column,
                              const IColumn& right_column, const UInt8* nested_null_map) const {
        // check array nested column type and get data
        const auto& src_data = reinterpret_cast<const NestedColumnType&>(nested_column).get_data();

        // check target column type and get data
        const auto& target_data = reinterpret_cast<const RightColumnType&>(right_column).get_data();

        PaddedPODArray<UInt8>* dst_null_map = nullptr;
        MutableColumnPtr array_nested_column = nullptr;
        IColumn* dst_column = nullptr;
        if (nested_null_map) {
            auto dst_nested_column =
                    ColumnNullable::create(nested_column.clone_empty(), ColumnUInt8::create());
            array_nested_column = dst_nested_column->get_ptr();
            dst_column = dst_nested_column->get_nested_column_ptr().get();
            dst_null_map = &dst_nested_column->get_null_map_data();
            dst_null_map->reserve(offsets.back());
        } else {
            auto dst_nested_column = nested_column.clone_empty();
            array_nested_column = dst_nested_column->get_ptr();
            dst_column = dst_nested_column.get();
        }

        auto& dst_data = reinterpret_cast<NestedColumnType&>(*dst_column).get_data();
        dst_data.reserve(offsets.back());

        auto dst_offsets_column = ColumnArray::ColumnOffsets::create();
        auto& dst_offsets = dst_offsets_column->get_data();
        dst_offsets.reserve(offsets.size());

        size_t cur = 0;
        for (size_t row = 0; row < offsets.size(); ++row) {
            size_t off = offsets[row - 1];
            size_t len = offsets[row] - off;

            if (len == 0) {
                // case: array:[], target:1 ==> []
                dst_offsets.push_back(cur);
                continue;
            }

            size_t count = 0;
            for (size_t pos = 0; pos < len; ++pos) {
                if (nested_null_map && nested_null_map[off + pos]) {
                    // case: array:[Null], target:1 ==> [Null]
                    dst_data.push_back(typename NestedColumnType::value_type());
                    dst_null_map->push_back(1);
                    continue;
                }

                if (src_data[off + pos] == target_data[row]) {
                    ++count;
                } else {
                    dst_data.push_back(src_data[off + pos]);
                    if (nested_null_map) {
                        dst_null_map->push_back(0);
                    }
                }
            }

            cur += len - count;
            dst_offsets.push_back(cur);
        }

        auto dst =
                ColumnArray::create(std::move(array_nested_column), std::move(dst_offsets_column));
        return dst;
    }

    ColumnPtr _execute_string(const ColumnArray::Offsets64& offsets, const IColumn& nested_column,
                              const IColumn& right_column, const UInt8* nested_null_map) const {
        // check array nested column type and get data
        const auto& src_offs = reinterpret_cast<const ColumnString&>(nested_column).get_offsets();
        const auto& src_chars = reinterpret_cast<const ColumnString&>(nested_column).get_chars();

        // check right column type and get data
        const auto& target_offs = reinterpret_cast<const ColumnString&>(right_column).get_offsets();
        const auto& target_chars = reinterpret_cast<const ColumnString&>(right_column).get_chars();

        PaddedPODArray<UInt8>* dst_null_map = nullptr;
        MutableColumnPtr array_nested_column = nullptr;
        IColumn* dst_column = nullptr;
        if (nested_null_map) {
            auto dst_nested_column =
                    ColumnNullable::create(nested_column.clone_empty(), ColumnUInt8::create());
            array_nested_column = dst_nested_column->get_ptr();
            dst_column = dst_nested_column->get_nested_column_ptr().get();
            dst_null_map = &dst_nested_column->get_null_map_data();
            dst_null_map->reserve(offsets.back());
        } else {
            auto dst_nested_column = nested_column.clone_empty();
            array_nested_column = dst_nested_column->get_ptr();
            dst_column = dst_nested_column.get();
        }

        auto& dst_offs = reinterpret_cast<ColumnString&>(*dst_column).get_offsets();
        auto& dst_chars = reinterpret_cast<ColumnString&>(*dst_column).get_chars();
        dst_offs.reserve(src_offs.size());
        dst_chars.reserve(src_offs.back());

        auto dst_offsets_column = ColumnArray::ColumnOffsets::create();
        auto& dst_offsets = dst_offsets_column->get_data();
        dst_offsets.reserve(offsets.size());

        size_t cur = 0;
        for (size_t row = 0; row < offsets.size(); ++row) {
            size_t off = offsets[row - 1];
            size_t len = offsets[row] - off;

            if (len == 0) {
                // case: array:[], target:'str' ==> []
                dst_offsets.push_back(cur);
                continue;
            }

            size_t target_off = target_offs[row - 1];
            size_t target_len = target_offs[row] - target_off;

            size_t count = 0;
            for (size_t pos = 0; pos < len; ++pos) {
                if (nested_null_map && nested_null_map[off + pos]) {
                    // case: array:[Null], target:'str' ==> [Null]
                    // dst_chars.push_back(0);
                    dst_offs.push_back(dst_offs.back());
                    dst_null_map->push_back(1);
                    continue;
                }

                size_t src_pos = src_offs[pos + off - 1];
                size_t src_len = src_offs[pos + off] - src_pos;
                const char* src_raw_v = reinterpret_cast<const char*>(&src_chars[src_pos]);
                const char* target_raw_v = reinterpret_cast<const char*>(&target_chars[target_off]);

                if (std::string_view(src_raw_v, src_len) ==
                    std::string_view(target_raw_v, target_len)) {
                    ++count;
                } else {
                    const size_t old_size = dst_chars.size();
                    const size_t new_size = old_size + src_len;
                    dst_chars.resize(new_size);
                    memcpy(&dst_chars[old_size], &src_chars[src_pos], src_len);
                    dst_offs.push_back(new_size);
                    if (nested_null_map) {
                        dst_null_map->push_back(0);
                    }
                }
            }

            cur += len - count;
            dst_offsets.push_back(cur);
        }

        auto dst =
                ColumnArray::create(std::move(array_nested_column), std::move(dst_offsets_column));
        return dst;
    }

    template <typename NestedColumnType>
    ColumnPtr _execute_number_expanded(const ColumnArray::Offsets64& offsets,
                                       const IColumn& nested_column, const IColumn& right_column,
                                       const UInt8* nested_null_map) const {
        if (is_column<NestedColumnType>(right_column)) {
            return _execute_number<NestedColumnType, NestedColumnType>(
                    offsets, nested_column, right_column, nested_null_map);
        }
        return nullptr;
    }

    ColumnPtr _execute_non_nullable(const ColumnsWithTypeAndName& arguments,
                                    size_t input_rows_count) const {
        // check array nested column type and get data
        auto left_column = arguments[0].column->convert_to_full_column_if_const();
        const auto& array_column = reinterpret_cast<const ColumnArray&>(*left_column);
        const auto& offsets = array_column.get_offsets();
        DCHECK(offsets.size() == input_rows_count);

        // check array right column type and get data
        const auto right_column = arguments[1].column->convert_to_full_column_if_const();

        const UInt8* nested_null_map = nullptr;
        ColumnPtr nested_column = nullptr;
        if (is_column_nullable(array_column.get_data())) {
            const auto& nested_null_column =
                    reinterpret_cast<const ColumnNullable&>(array_column.get_data());
            nested_null_map = nested_null_column.get_null_map_column().get_data().data();
            nested_column = nested_null_column.get_nested_column_ptr();
        } else {
            nested_column = array_column.get_data_ptr();
        }

        auto left_element_type = remove_nullable(
                assert_cast<const DataTypeArray&>(*arguments[0].type).get_nested_type());
        auto right_type = remove_nullable((arguments[1]).type);

        ColumnPtr res = nullptr;
        if (is_string_type(right_type->get_primitive_type()) &&
            is_string_type(left_element_type->get_primitive_type())) {
            res = _execute_string(offsets, *nested_column, *right_column, nested_null_map);
        } else if (is_number(right_type->get_primitive_type()) &&
                   is_number(left_element_type->get_primitive_type())) {
            switch (left_element_type->get_primitive_type()) {
            case TYPE_BOOLEAN:
                res = _execute_number_expanded<ColumnUInt8>(offsets, *nested_column, *right_column,
                                                            nested_null_map);
                break;
            case TYPE_TINYINT:
                res = _execute_number_expanded<ColumnInt8>(offsets, *nested_column, *right_column,
                                                           nested_null_map);
                break;
            case TYPE_SMALLINT:
                res = _execute_number_expanded<ColumnInt16>(offsets, *nested_column, *right_column,
                                                            nested_null_map);
                break;
            case TYPE_INT:
                res = _execute_number_expanded<ColumnInt32>(offsets, *nested_column, *right_column,
                                                            nested_null_map);
                break;
            case TYPE_BIGINT:
                res = _execute_number_expanded<ColumnInt64>(offsets, *nested_column, *right_column,
                                                            nested_null_map);
                break;
            case TYPE_LARGEINT:
                res = _execute_number_expanded<ColumnInt128>(offsets, *nested_column, *right_column,
                                                             nested_null_map);
                break;
            case TYPE_FLOAT:
                res = _execute_number_expanded<ColumnFloat32>(offsets, *nested_column,
                                                              *right_column, nested_null_map);
                break;
            case TYPE_DOUBLE:
                res = _execute_number_expanded<ColumnFloat64>(offsets, *nested_column,
                                                              *right_column, nested_null_map);
                break;
            case TYPE_DECIMAL32:
                res = _execute_number_expanded<ColumnDecimal32>(offsets, *nested_column,
                                                                *right_column, nested_null_map);
                break;
            case TYPE_DECIMAL64:
                res = _execute_number_expanded<ColumnDecimal64>(offsets, *nested_column,
                                                                *right_column, nested_null_map);
                break;
            case TYPE_DECIMAL128I:
                res = _execute_number_expanded<ColumnDecimal128V3>(offsets, *nested_column,
                                                                   *right_column, nested_null_map);
                break;
            case TYPE_DECIMALV2:
                res = _execute_number_expanded<ColumnDecimal128V2>(offsets, *nested_column,
                                                                   *right_column, nested_null_map);
                break;
            case TYPE_DECIMAL256:
                res = _execute_number_expanded<ColumnDecimal256>(offsets, *nested_column,
                                                                 *right_column, nested_null_map);
                break;
            default:
                break;
            }
        } else if (is_date_or_datetime(right_type->get_primitive_type()) &&
                   is_date_or_datetime(left_element_type->get_primitive_type())) {
            if (left_element_type->get_primitive_type() == PrimitiveType::TYPE_DATE) {
                res = _execute_number_expanded<ColumnDate>(offsets, *nested_column, *right_column,
                                                           nested_null_map);
            } else if (left_element_type->get_primitive_type() == PrimitiveType::TYPE_DATETIME) {
                res = _execute_number_expanded<ColumnDateTime>(offsets, *nested_column,
                                                               *right_column, nested_null_map);
            }
        } else if (is_date_v2_or_datetime_v2(right_type->get_primitive_type()) &&
                   is_date_v2_or_datetime_v2(left_element_type->get_primitive_type())) {
            if (left_element_type->get_primitive_type() == PrimitiveType::TYPE_DATEV2) {
                res = _execute_number_expanded<ColumnDateV2>(offsets, *nested_column, *right_column,
                                                             nested_null_map);
            } else if (left_element_type->get_primitive_type() == PrimitiveType::TYPE_DATETIMEV2) {
                res = _execute_number_expanded<ColumnDateTimeV2>(offsets, *nested_column,
                                                                 *right_column, nested_null_map);
            }
        }
        return res;
    }
};

} // namespace doris::vectorized
