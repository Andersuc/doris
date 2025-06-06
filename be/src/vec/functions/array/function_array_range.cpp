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

#include <glog/logging.h>

#include <algorithm>
#include <boost/iterator/iterator_facade.hpp>
#include <cstddef>
#include <memory>
#include <utility>

#include "common/status.h"
#include "vec/aggregate_functions/aggregate_function.h"
#include "vec/columns/column.h"
#include "vec/columns/column_array.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_vector.h"
#include "vec/common/assert_cast.h"
#include "vec/common/pod_array_fwd.h"
#include "vec/core/block.h"
#include "vec/core/column_numbers.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/types.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_array.h"
#include "vec/data_types/data_type_date_or_datetime_v2.h"
#include "vec/data_types/data_type_date_time.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_number.h"
#include "vec/functions/function.h"
#include "vec/functions/function_date_or_datetime_computation.h"
#include "vec/functions/simple_function_factory.h"
#include "vec/runtime/vdatetime_value.h"

namespace doris {
class FunctionContext;
} // namespace doris

namespace doris::vectorized {
#include "common/compile_check_begin.h"

template <typename Impl>
class FunctionArrayRange : public IFunction {
public:
    static constexpr auto name = Impl::name;

    static FunctionPtr create() { return std::make_shared<FunctionArrayRange>(); }

    /// Get function name.
    String get_name() const override { return name; }

    bool is_variadic() const override { return true; }

    size_t get_number_of_arguments() const override {
        return get_variadic_argument_types_impl().size();
    }

    DataTypes get_variadic_argument_types_impl() const override {
        return Impl::get_variadic_argument_types();
    }

    DataTypePtr get_return_type_impl(const DataTypes& arguments) const override {
        auto nested_type = make_nullable(Impl::get_data_type());
        auto res = std::make_shared<DataTypeArray>(nested_type);
        return make_nullable(res);
    }

    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const override {
        return Impl::execute_impl(context, block, arguments, result, input_rows_count);
    }
};

template <PrimitiveType SourceDataPType, typename TimeUnitOrVoid = void>
struct RangeImplUtil {
    using SourceDataType = typename PrimitiveTypeTraits<SourceDataPType>::ColumnItemType;
    using DataType = typename PrimitiveTypeTraits<SourceDataPType>::DataType;

    static DataTypePtr get_data_type() { return std::make_shared<DataType>(); }

    static constexpr const char* get_function_name() {
        if constexpr (std::is_same_v<SourceDataType, DateTimeV2> &&
                      !std::is_same_v<TimeUnitOrVoid, void>) {
            if constexpr (std::is_same_v<TimeUnitOrVoid,
                                         std::integral_constant<TimeUnit, TimeUnit::YEAR>>) {
                return "array_range_year_unit";
            } else if constexpr (std::is_same_v<
                                         TimeUnitOrVoid,
                                         std::integral_constant<TimeUnit, TimeUnit::MONTH>>) {
                return "array_range_month_unit";
            } else if constexpr (std::is_same_v<TimeUnitOrVoid,
                                                std::integral_constant<TimeUnit, TimeUnit::WEEK>>) {
                return "array_range_week_unit";
            } else if constexpr (std::is_same_v<TimeUnitOrVoid,
                                                std::integral_constant<TimeUnit, TimeUnit::DAY>>) {
                return "array_range_day_unit";
            } else if constexpr (std::is_same_v<TimeUnitOrVoid,
                                                std::integral_constant<TimeUnit, TimeUnit::HOUR>>) {
                return "array_range_hour_unit";
            } else if constexpr (std::is_same_v<
                                         TimeUnitOrVoid,
                                         std::integral_constant<TimeUnit, TimeUnit::MINUTE>>) {
                return "array_range_minute_unit";
            } else if constexpr (std::is_same_v<
                                         TimeUnitOrVoid,
                                         std::integral_constant<TimeUnit, TimeUnit::SECOND>>) {
                return "array_range_second_unit";
            }
        } else {
            return "array_range";
        }
    }

    static constexpr auto name = get_function_name();

    static Status range_execute(Block& block, const ColumnNumbers& arguments, uint32_t result,
                                size_t input_rows_count) {
        DCHECK_EQ(arguments.size(), 3);
        auto return_nested_type = make_nullable(std::make_shared<DataType>());
        auto dest_array_column_ptr = ColumnArray::create(return_nested_type->create_column(),
                                                         ColumnArray::ColumnOffsets::create());
        IColumn* dest_nested_column = &dest_array_column_ptr->get_data();
        auto* dest_nested_nullable_col = assert_cast<ColumnNullable*>(dest_nested_column);
        dest_nested_column = dest_nested_nullable_col->get_nested_column_ptr().get();
        auto& dest_nested_null_map = dest_nested_nullable_col->get_null_map_column().get_data();

        auto args_null_map = ColumnUInt8::create(input_rows_count, 0);
        ColumnPtr argument_columns[3];
        for (int i = 0; i < 3; ++i) {
            argument_columns[i] =
                    block.get_by_position(arguments[i]).column->convert_to_full_column_if_const();
        }
        auto start_column =
                assert_cast<const ColumnVector<SourceDataPType>*>(argument_columns[0].get());
        auto end_column =
                assert_cast<const ColumnVector<SourceDataPType>*>(argument_columns[1].get());
        const auto* step_column = assert_cast<const ColumnInt32*>(argument_columns[2].get());

        DCHECK(dest_nested_column != nullptr);
        auto& dest_offsets = dest_array_column_ptr->get_offsets();
        auto nested_column = reinterpret_cast<ColumnVector<SourceDataPType>*>(dest_nested_column);
        dest_offsets.reserve(input_rows_count);
        dest_nested_column->reserve(input_rows_count);
        dest_nested_null_map.reserve(input_rows_count);

        RETURN_IF_ERROR(vector(start_column->get_data(), end_column->get_data(),
                               step_column->get_data(), args_null_map->get_data(),
                               nested_column->get_data(), dest_nested_null_map, dest_offsets));

        block.get_by_position(result).column =
                ColumnNullable::create(std::move(dest_array_column_ptr), std::move(args_null_map));
        return Status::OK();
    }

private:
    static Status vector(const PaddedPODArray<SourceDataType>& start,
                         const PaddedPODArray<SourceDataType>& end,
                         const PaddedPODArray<Int32>& step, NullMap& args_null_map,
                         PaddedPODArray<SourceDataType>& nested_column,
                         PaddedPODArray<UInt8>& dest_nested_null_map,
                         ColumnArray::Offsets64& dest_offsets) {
        size_t rows = start.size();
        for (auto row = 0; row < rows; ++row) {
            auto idx = start[row];
            auto end_row = end[row];
            auto step_row = step[row];
            auto args_null_map_row = args_null_map[row];
            if constexpr (std::is_same_v<SourceDataType, Int32>) {
                if (args_null_map_row || idx < 0 || end_row < 0 || step_row <= 0) {
                    args_null_map[row] = 1;
                    dest_offsets.push_back(dest_offsets.back());
                    continue;
                } else {
                    if (idx < end_row && step_row > 0 &&
                        ((static_cast<__int128_t>(end_row) - static_cast<__int128_t>(idx) - 1) /
                                 static_cast<__int128_t>(step_row) +
                         1) > max_array_size_as_field) {
                        return Status::InvalidArgument("Array size exceeds the limit {}",
                                                       max_array_size_as_field);
                    }
                    size_t offset = dest_offsets.back();
                    while (idx < end[row]) {
                        nested_column.push_back(idx);
                        dest_nested_null_map.push_back(0);
                        offset++;
                        idx = idx + step_row;
                    }
                    dest_offsets.push_back(offset);
                }
            } else {
                const auto& idx_0 = reinterpret_cast<const DateV2Value<DateTimeV2ValueType>&>(idx);
                const auto& end_row_cast =
                        reinterpret_cast<const DateV2Value<DateTimeV2ValueType>&>(end_row);
                bool is_null = !idx_0.is_valid_date();
                bool is_end_row_invalid = !end_row_cast.is_valid_date();
                if (args_null_map_row || step_row <= 0 || is_null || is_end_row_invalid) {
                    args_null_map[row] = 1;
                    dest_offsets.push_back(dest_offsets.back());
                    continue;
                } else {
                    size_t offset = dest_offsets.back();
                    using UNIT = std::conditional_t<std::is_same_v<TimeUnitOrVoid, void>,
                                                    std::integral_constant<TimeUnit, TimeUnit::DAY>,
                                                    TimeUnitOrVoid>;
                    int move = 0;
                    while (doris::datetime_diff<UNIT::value, DateTimeV2ValueType,
                                                DateTimeV2ValueType>(idx, end_row) > 0) {
                        if (move > max_array_size_as_field) {
                            return Status::InvalidArgument("Array size exceeds the limit {}",
                                                           max_array_size_as_field);
                        }
                        nested_column.push_back(idx);
                        dest_nested_null_map.push_back(0);
                        offset++;
                        move++;
                        idx = doris::vectorized::date_time_add<UNIT::value, TYPE_DATETIMEV2,
                                                               TYPE_DATETIMEV2>(idx, step_row,
                                                                                is_null);
                    }
                    dest_offsets.push_back(offset);
                }
            }
        }
        return Status::OK();
    }
};

template <PrimitiveType SourceDataPType, typename TimeUnitOrVoid = void>
struct RangeOneImpl : public RangeImplUtil<SourceDataPType, TimeUnitOrVoid> {
    static DataTypes get_variadic_argument_types() {
        return {std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>()};
    }

    static Status execute_impl(FunctionContext* context, Block& block,
                               const ColumnNumbers& arguments, uint32_t result,
                               size_t input_rows_count) {
        using ColumnType =
                std::conditional_t<SourceDataPType == TYPE_INT, ColumnInt32, ColumnDateTimeV2>;
        auto start_column = ColumnType::create(input_rows_count, 0);
        auto step_column = ColumnInt32::create(input_rows_count, 1);
        block.insert({std::move(start_column),
                      std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>(),
                      "start_column"});
        block.insert({std::move(step_column), std::make_shared<DataTypeInt32>(), "step_column"});
        ColumnNumbers temp_arguments = {block.columns() - 2, arguments[0], block.columns() - 1};
        return (RangeImplUtil<SourceDataPType, TimeUnitOrVoid>::range_execute)(
                block, temp_arguments, result, input_rows_count);
    }
};

template <PrimitiveType SourceDataPType, typename TimeUnitOrVoid = void>
struct RangeTwoImpl : public RangeImplUtil<SourceDataPType, TimeUnitOrVoid> {
    static DataTypes get_variadic_argument_types() {
        return {std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>(),
                std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>()};
    }

    static Status execute_impl(FunctionContext* context, Block& block,
                               const ColumnNumbers& arguments, uint32_t result,
                               size_t input_rows_count) {
        auto step_column = ColumnInt32::create(input_rows_count, 1);
        block.insert({std::move(step_column), std::make_shared<DataTypeInt32>(), "step_column"});
        ColumnNumbers temp_arguments = {arguments[0], arguments[1], block.columns() - 1};
        return (RangeImplUtil<SourceDataPType, TimeUnitOrVoid>::range_execute)(
                block, temp_arguments, result, input_rows_count);
    }
};

template <PrimitiveType SourceDataPType, typename TimeUnitOrVoid = void>
struct RangeThreeImpl : public RangeImplUtil<SourceDataPType, TimeUnitOrVoid> {
    static DataTypes get_variadic_argument_types() {
        return {std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>(),
                std::make_shared<typename RangeImplUtil<SourceDataPType>::DataType>(),
                std::make_shared<DataTypeInt32>()};
    }

    static Status execute_impl(FunctionContext* context, Block& block,
                               const ColumnNumbers& arguments, uint32_t result,
                               size_t input_rows_count) {
        return (RangeImplUtil<SourceDataPType, TimeUnitOrVoid>::range_execute)(
                block, arguments, result, input_rows_count);
    }
};

void register_function_array_range(SimpleFunctionFactory& factory) {
    /// One argument, just for Int32
    factory.register_function<FunctionArrayRange<RangeOneImpl<TYPE_INT>>>();

    /// Two arguments, for Int32 and DateTimeV2 without Interval
    factory.register_function<FunctionArrayRange<RangeTwoImpl<TYPE_INT>>>();
    factory.register_function<FunctionArrayRange<RangeTwoImpl<TYPE_DATETIMEV2>>>();

    /// Three arguments, for Int32 and DateTimeV2 with YEAR to SECOND Interval
    factory.register_function<FunctionArrayRange<RangeThreeImpl<TYPE_INT>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::YEAR>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::MONTH>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::WEEK>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::DAY>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::HOUR>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::MINUTE>>>>();
    factory.register_function<FunctionArrayRange<
            RangeThreeImpl<TYPE_DATETIMEV2, std::integral_constant<TimeUnit, TimeUnit::SECOND>>>>();

    // alias
    factory.register_alias("array_range", "sequence");
}

} // namespace doris::vectorized
