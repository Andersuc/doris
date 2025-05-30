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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/cast_set.h"
#include "common/status.h"
#include "util/encryption_util.h"
#include "vec/columns/column.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_string.h"
#include "vec/columns/column_vector.h"
#include "vec/common/assert_cast.h"
#include "vec/common/string_ref.h"
#include "vec/core/block.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_string.h"
#include "vec/functions/function.h"
#include "vec/functions/simple_function_factory.h"
#include "vec/utils/stringop_substring.h"
#include "vec/utils/util.hpp"

namespace doris {
#include "common/compile_check_begin.h"
class FunctionContext;
} // namespace doris

namespace doris::vectorized {

inline StringCaseUnorderedMap<EncryptionMode> aes_mode_map {
        {"AES_128_ECB", EncryptionMode::AES_128_ECB},
        {"AES_192_ECB", EncryptionMode::AES_192_ECB},
        {"AES_256_ECB", EncryptionMode::AES_256_ECB},
        {"AES_128_CBC", EncryptionMode::AES_128_CBC},
        {"AES_192_CBC", EncryptionMode::AES_192_CBC},
        {"AES_256_CBC", EncryptionMode::AES_256_CBC},
        {"AES_128_CFB", EncryptionMode::AES_128_CFB},
        {"AES_192_CFB", EncryptionMode::AES_192_CFB},
        {"AES_256_CFB", EncryptionMode::AES_256_CFB},
        {"AES_128_CFB1", EncryptionMode::AES_128_CFB1},
        {"AES_192_CFB1", EncryptionMode::AES_192_CFB1},
        {"AES_256_CFB1", EncryptionMode::AES_256_CFB1},
        {"AES_128_CFB8", EncryptionMode::AES_128_CFB8},
        {"AES_192_CFB8", EncryptionMode::AES_192_CFB8},
        {"AES_256_CFB8", EncryptionMode::AES_256_CFB8},
        {"AES_128_CFB128", EncryptionMode::AES_128_CFB128},
        {"AES_192_CFB128", EncryptionMode::AES_192_CFB128},
        {"AES_256_CFB128", EncryptionMode::AES_256_CFB128},
        {"AES_128_CTR", EncryptionMode::AES_128_CTR},
        {"AES_192_CTR", EncryptionMode::AES_192_CTR},
        {"AES_256_CTR", EncryptionMode::AES_256_CTR},
        {"AES_128_OFB", EncryptionMode::AES_128_OFB},
        {"AES_192_OFB", EncryptionMode::AES_192_OFB},
        {"AES_256_OFB", EncryptionMode::AES_256_OFB},
        {"AES_128_GCM", EncryptionMode::AES_128_GCM},
        {"AES_192_GCM", EncryptionMode::AES_192_GCM},
        {"AES_256_GCM", EncryptionMode::AES_256_GCM}};
inline StringCaseUnorderedMap<EncryptionMode> sm4_mode_map {
        {"SM4_128_ECB", EncryptionMode::SM4_128_ECB},
        {"SM4_128_CBC", EncryptionMode::SM4_128_CBC},
        {"SM4_128_CFB128", EncryptionMode::SM4_128_CFB128},
        {"SM4_128_OFB", EncryptionMode::SM4_128_OFB},
        {"SM4_128_CTR", EncryptionMode::SM4_128_CTR}};
template <typename Impl, typename FunctionName>
class FunctionEncryptionAndDecrypt : public IFunction {
public:
    static constexpr auto name = FunctionName::name;

    String get_name() const override { return name; }

    static FunctionPtr create() { return std::make_shared<FunctionEncryptionAndDecrypt>(); }

    DataTypePtr get_return_type_impl(const DataTypes& arguments) const override {
        return make_nullable(std::make_shared<DataTypeString>());
    }

    DataTypes get_variadic_argument_types_impl() const override {
        return Impl::get_variadic_argument_types_impl();
    }

    size_t get_number_of_arguments() const override {
        return get_variadic_argument_types_impl().size();
    }

    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const override {
        return Impl::execute_impl_inner(context, block, arguments, result, input_rows_count);
    }
};

template <typename Impl, bool is_encrypt>
void execute_result_vector(std::vector<const ColumnString::Offsets*>& offsets_list,
                           std::vector<const ColumnString::Chars*>& chars_list, size_t i,
                           EncryptionMode& encryption_mode, const char* iv_raw, int iv_length,
                           ColumnString::Chars& result_data, ColumnString::Offsets& result_offset,
                           NullMap& null_map, const char* aad, int aad_length) {
    int src_size = (*offsets_list[0])[i] - (*offsets_list[0])[i - 1];
    const auto* src_raw =
            reinterpret_cast<const char*>(&(*chars_list[0])[(*offsets_list[0])[i - 1]]);
    int key_size = (*offsets_list[1])[i] - (*offsets_list[1])[i - 1];
    const auto* key_raw =
            reinterpret_cast<const char*>(&(*chars_list[1])[(*offsets_list[1])[i - 1]]);
    execute_result<Impl, is_encrypt>(src_raw, src_size, key_raw, key_size, i, encryption_mode,
                                     iv_raw, iv_length, result_data, result_offset, null_map, aad,
                                     aad_length);
}

template <typename Impl, bool is_encrypt>
void execute_result_const(const ColumnString::Offsets* offsets_column,
                          const ColumnString::Chars* chars_column, StringRef key_arg, size_t i,
                          EncryptionMode& encryption_mode, const char* iv_raw, size_t iv_length,
                          ColumnString::Chars& result_data, ColumnString::Offsets& result_offset,
                          NullMap& null_map, const char* aad, size_t aad_length) {
    int src_size = (*offsets_column)[i] - (*offsets_column)[i - 1];
    const auto* src_raw = reinterpret_cast<const char*>(&(*chars_column)[(*offsets_column)[i - 1]]);
    execute_result<Impl, is_encrypt>(src_raw, src_size, key_arg.data, key_arg.size, i,
                                     encryption_mode, iv_raw, iv_length, result_data, result_offset,
                                     null_map, aad, aad_length);
}

template <typename Impl, bool is_encrypt>
void execute_result(const char* src_raw, size_t src_size, const char* key_raw, size_t key_size,
                    size_t i, EncryptionMode& encryption_mode, const char* iv_raw, size_t iv_length,
                    ColumnString::Chars& result_data, ColumnString::Offsets& result_offset,
                    NullMap& null_map, const char* aad, size_t aad_length) {
    auto cipher_len = src_size;
    if constexpr (is_encrypt) {
        cipher_len += 16;
        // for output AEAD tag
        if (EncryptionUtil::is_gcm_mode(encryption_mode)) {
            cipher_len += EncryptionUtil::GCM_TAG_SIZE;
        }
    }
    std::unique_ptr<char[]> p;
    p.reset(new char[cipher_len]);
    int ret_code = 0;

    ret_code = Impl::execute_impl(encryption_mode, (unsigned char*)src_raw, src_size,
                                  (unsigned char*)key_raw, key_size, iv_raw, iv_length, true,
                                  (unsigned char*)p.get(), (unsigned char*)aad, aad_length);

    if (ret_code < 0) {
        StringOP::push_null_string(i, result_data, result_offset, null_map);
    } else {
        StringOP::push_value_string(std::string_view(p.get(), ret_code), i, result_data,
                                    result_offset);
    }
}

template <typename Impl, EncryptionMode mode, bool is_encrypt>
struct EncryptionAndDecryptTwoImpl {
    static DataTypes get_variadic_argument_types_impl() {
        return {std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>(),
                std::make_shared<DataTypeString>()};
    }

    static Status execute_impl_inner(FunctionContext* context, Block& block,
                                     const ColumnNumbers& arguments, uint32_t result,
                                     size_t input_rows_count) {
        auto result_column = ColumnString::create();
        auto result_null_map_column = ColumnUInt8::create(input_rows_count, 0);
        DCHECK_EQ(3, arguments.size());
        const size_t argument_size = 3;
        bool col_const[argument_size];
        ColumnPtr argument_columns[argument_size];
        for (int i = 0; i < argument_size; ++i) {
            col_const[i] = is_column_const(*block.get_by_position(arguments[i]).column);
        }
        argument_columns[0] = col_const[0] ? static_cast<const ColumnConst&>(
                                                     *block.get_by_position(arguments[0]).column)
                                                     .convert_to_full_column()
                                           : block.get_by_position(arguments[0]).column;

        default_preprocess_parameter_columns(argument_columns, col_const, {1, 2}, block, arguments);

        auto& result_data = result_column->get_chars();
        auto& result_offset = result_column->get_offsets();
        result_offset.resize(input_rows_count);

        if (col_const[1] && col_const[2]) {
            vector_const(assert_cast<const ColumnString*>(argument_columns[0].get()),
                         argument_columns[1]->get_data_at(0), argument_columns[2]->get_data_at(0),
                         input_rows_count, result_data, result_offset,
                         result_null_map_column->get_data());
        } else {
            std::vector<const ColumnString::Offsets*> offsets_list(argument_size);
            std::vector<const ColumnString::Chars*> chars_list(argument_size);
            for (size_t i = 0; i < argument_size; ++i) {
                const auto* col_str = assert_cast<const ColumnString*>(argument_columns[i].get());
                offsets_list[i] = &col_str->get_offsets();
                chars_list[i] = &col_str->get_chars();
            }
            vector_vector(offsets_list, chars_list, input_rows_count, result_data, result_offset,
                          result_null_map_column->get_data());
        }
        block.get_by_position(result).column =
                ColumnNullable::create(std::move(result_column), std::move(result_null_map_column));
        return Status::OK();
    }

    static void vector_const(const ColumnString* column, StringRef key_arg, StringRef mode_arg,
                             size_t input_rows_count, ColumnString::Chars& result_data,
                             ColumnString::Offsets& result_offset, NullMap& null_map) {
        EncryptionMode encryption_mode = mode;
        std::string mode_str(mode_arg.data, mode_arg.size);
        bool all_insert_null = false;
        if (mode_arg.size != 0) {
            if (!aes_mode_map.contains(mode_str)) {
                all_insert_null = true;
            } else {
                encryption_mode = aes_mode_map.at(mode_str);
            }
        }
        const ColumnString::Offsets* offsets_column = &column->get_offsets();
        const ColumnString::Chars* chars_column = &column->get_chars();
        for (int i = 0; i < input_rows_count; ++i) {
            if (all_insert_null || null_map[i]) {
                StringOP::push_null_string(i, result_data, result_offset, null_map);
                continue;
            }
            execute_result_const<Impl, is_encrypt>(offsets_column, chars_column, key_arg, i,
                                                   encryption_mode, nullptr, 0, result_data,
                                                   result_offset, null_map, nullptr, 0);
        }
    }

    static void vector_vector(std::vector<const ColumnString::Offsets*>& offsets_list,
                              std::vector<const ColumnString::Chars*>& chars_list,
                              size_t input_rows_count, ColumnString::Chars& result_data,
                              ColumnString::Offsets& result_offset, NullMap& null_map) {
        for (int i = 0; i < input_rows_count; ++i) {
            if (null_map[i]) {
                StringOP::push_null_string(i, result_data, result_offset, null_map);
                continue;
            }
            EncryptionMode encryption_mode = mode;
            int mode_size = (*offsets_list[2])[i] - (*offsets_list[2])[i - 1];
            const auto* mode_raw =
                    reinterpret_cast<const char*>(&(*chars_list[2])[(*offsets_list[2])[i - 1]]);
            if (mode_size != 0) {
                std::string mode_str(mode_raw, mode_size);
                if (aes_mode_map.count(mode_str) == 0) {
                    StringOP::push_null_string(i, result_data, result_offset, null_map);
                    continue;
                }
                encryption_mode = aes_mode_map.at(mode_str);
            }
            execute_result_vector<Impl, is_encrypt>(offsets_list, chars_list, i, encryption_mode,
                                                    nullptr, 0, result_data, result_offset,
                                                    null_map, nullptr, 0);
        }
    }
};

template <typename Impl, EncryptionMode mode, bool is_encrypt, bool is_sm_mode, int arg_num = 4>
struct EncryptionAndDecryptMultiImpl {
    static DataTypes get_variadic_argument_types_impl() {
        if constexpr (arg_num == 5) {
            return {std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>(),
                    std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>(),
                    std::make_shared<DataTypeString>()};
        } else {
            return {std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>(),
                    std::make_shared<DataTypeString>(), std::make_shared<DataTypeString>()};
        }
    }

    static Status execute_impl_inner(FunctionContext* context, Block& block,
                                     const ColumnNumbers& arguments, uint32_t result,
                                     size_t input_rows_count) {
        auto result_column = ColumnString::create();
        auto result_null_map_column = ColumnUInt8::create(input_rows_count, 0);
        DCHECK_EQ(arguments.size(), arg_num);
        const size_t argument_size = arg_num;
        bool col_const[argument_size];
        ColumnPtr argument_columns[argument_size];
        for (int i = 0; i < argument_size; ++i) {
            col_const[i] = is_column_const(*block.get_by_position(arguments[i]).column);
        }
        argument_columns[0] = col_const[0] ? static_cast<const ColumnConst&>(
                                                     *block.get_by_position(arguments[0]).column)
                                                     .convert_to_full_column()
                                           : block.get_by_position(arguments[0]).column;

        if constexpr (arg_num == 5) {
            default_preprocess_parameter_columns(argument_columns, col_const, {1, 2, 3, 4}, block,
                                                 arguments);
        } else {
            default_preprocess_parameter_columns(argument_columns, col_const, {1, 2, 3}, block,
                                                 arguments);
        }

        auto& result_data = result_column->get_chars();
        auto& result_offset = result_column->get_offsets();
        result_offset.resize(input_rows_count);

        if ((arg_num == 5) && col_const[1] && col_const[2] && col_const[3] && col_const[4]) {
            vector_const(assert_cast<const ColumnString*>(argument_columns[0].get()),
                         argument_columns[1]->get_data_at(0), argument_columns[2]->get_data_at(0),
                         argument_columns[3]->get_data_at(0), input_rows_count, result_data,
                         result_offset, result_null_map_column->get_data(),
                         argument_columns[4]->get_data_at(0));
        } else if ((arg_num == 4) && col_const[1] && col_const[2] && col_const[3]) {
            vector_const(assert_cast<const ColumnString*>(argument_columns[0].get()),
                         argument_columns[1]->get_data_at(0), argument_columns[2]->get_data_at(0),
                         argument_columns[3]->get_data_at(0), input_rows_count, result_data,
                         result_offset, result_null_map_column->get_data(), StringRef());
        } else {
            std::vector<const ColumnString::Offsets*> offsets_list(argument_size);
            std::vector<const ColumnString::Chars*> chars_list(argument_size);
            for (size_t i = 0; i < argument_size; ++i) {
                const auto* col_str = assert_cast<const ColumnString*>(argument_columns[i].get());
                offsets_list[i] = &col_str->get_offsets();
                chars_list[i] = &col_str->get_chars();
            }
            vector_vector(offsets_list, chars_list, input_rows_count, result_data, result_offset,
                          result_null_map_column->get_data());
        }
        block.get_by_position(result).column =
                ColumnNullable::create(std::move(result_column), std::move(result_null_map_column));
        return Status::OK();
    }

    static void vector_const(const ColumnString* column, StringRef key_arg, StringRef iv_arg,
                             StringRef mode_arg, size_t input_rows_count,
                             ColumnString::Chars& result_data, ColumnString::Offsets& result_offset,
                             NullMap& null_map, StringRef aad_arg) {
        EncryptionMode encryption_mode = mode;
        bool all_insert_null = false;
        if (mode_arg.size != 0) {
            std::string mode_str(mode_arg.data, mode_arg.size);
            if constexpr (is_sm_mode) {
                if (sm4_mode_map.count(mode_str) == 0) {
                    all_insert_null = true;
                } else {
                    encryption_mode = sm4_mode_map.at(mode_str);
                }
            } else {
                if (aes_mode_map.count(mode_str) == 0) {
                    all_insert_null = true;
                } else {
                    encryption_mode = aes_mode_map.at(mode_str);
                }
            }
        }

        const ColumnString::Offsets* offsets_column = &column->get_offsets();
        const ColumnString::Chars* chars_column = &column->get_chars();
        for (int i = 0; i < input_rows_count; ++i) {
            if (all_insert_null || null_map[i]) {
                StringOP::push_null_string(i, result_data, result_offset, null_map);
                continue;
            }
            execute_result_const<Impl, is_encrypt>(
                    offsets_column, chars_column, key_arg, i, encryption_mode, iv_arg.data,
                    iv_arg.size, result_data, result_offset, null_map, aad_arg.data, aad_arg.size);
        }
    }

    static void vector_vector(std::vector<const ColumnString::Offsets*>& offsets_list,
                              std::vector<const ColumnString::Chars*>& chars_list,
                              size_t input_rows_count, ColumnString::Chars& result_data,
                              ColumnString::Offsets& result_offset, NullMap& null_map) {
        for (int i = 0; i < input_rows_count; ++i) {
            if (null_map[i]) {
                StringOP::push_null_string(i, result_data, result_offset, null_map);
                continue;
            }

            EncryptionMode encryption_mode = mode;
            int mode_size = (*offsets_list[3])[i] - (*offsets_list[3])[i - 1];
            int iv_size = (*offsets_list[2])[i] - (*offsets_list[2])[i - 1];
            const auto* mode_raw =
                    reinterpret_cast<const char*>(&(*chars_list[3])[(*offsets_list[3])[i - 1]]);
            const auto* iv_raw =
                    reinterpret_cast<const char*>(&(*chars_list[2])[(*offsets_list[2])[i - 1]]);
            if (mode_size != 0) {
                std::string mode_str(mode_raw, mode_size);
                if constexpr (is_sm_mode) {
                    if (sm4_mode_map.count(mode_str) == 0) {
                        StringOP::push_null_string(i, result_data, result_offset, null_map);
                        continue;
                    }
                    encryption_mode = sm4_mode_map.at(mode_str);
                } else {
                    if (aes_mode_map.count(mode_str) == 0) {
                        StringOP::push_null_string(i, result_data, result_offset, null_map);
                        continue;
                    }
                    encryption_mode = aes_mode_map.at(mode_str);
                }
            }

            int aad_size = 0;
            const char* aad = nullptr;
            if constexpr (arg_num == 5) {
                aad_size = (*offsets_list[4])[i] - (*offsets_list[4])[i - 1];
                aad = reinterpret_cast<const char*>(&(*chars_list[4])[(*offsets_list[4])[i - 1]]);
            }

            execute_result_vector<Impl, is_encrypt>(offsets_list, chars_list, i, encryption_mode,
                                                    iv_raw, iv_size, result_data, result_offset,
                                                    null_map, aad, aad_size);
        }
    }
};

struct EncryptImpl {
    static int execute_impl(EncryptionMode mode, const unsigned char* source, size_t source_length,
                            const unsigned char* key, size_t key_length, const char* iv,
                            size_t iv_length, bool padding, unsigned char* encrypt,
                            const unsigned char* aad, size_t aad_length) {
        // now the openssl only support int, so here we need to cast size_t to uint32_t
        return EncryptionUtil::encrypt(mode, source, cast_set<uint32_t>(source_length), key,
                                       cast_set<uint32_t>(key_length), iv, cast_set<int>(iv_length),
                                       true, encrypt, aad, cast_set<uint32_t>(aad_length));
    }
};

struct DecryptImpl {
    static int execute_impl(EncryptionMode mode, const unsigned char* source, size_t source_length,
                            const unsigned char* key, size_t key_length, const char* iv,
                            size_t iv_length, bool padding, unsigned char* encrypt,
                            const unsigned char* aad, size_t aad_length) {
        return EncryptionUtil::decrypt(mode, source, cast_set<uint32_t>(source_length), key,
                                       cast_set<uint32_t>(key_length), iv, cast_set<int>(iv_length),
                                       true, encrypt, aad, cast_set<uint32_t>(aad_length));
    }
};

struct SM4EncryptName {
    static constexpr auto name = "sm4_encrypt";
};

struct SM4DecryptName {
    static constexpr auto name = "sm4_decrypt";
};

struct AESEncryptName {
    static constexpr auto name = "aes_encrypt";
};

struct AESDecryptName {
    static constexpr auto name = "aes_decrypt";
};

void register_function_encryption(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptTwoImpl<EncryptImpl, EncryptionMode::SM4_128_ECB, true>,
            SM4EncryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptTwoImpl<DecryptImpl, EncryptionMode::SM4_128_ECB, false>,
            SM4DecryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptTwoImpl<EncryptImpl, EncryptionMode::AES_128_ECB, true>,
            AESEncryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptTwoImpl<DecryptImpl, EncryptionMode::AES_128_ECB, false>,
            AESDecryptName>>();

    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<EncryptImpl, EncryptionMode::SM4_128_ECB, true, true>,
            SM4EncryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<DecryptImpl, EncryptionMode::SM4_128_ECB, false, true>,
            SM4DecryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<EncryptImpl, EncryptionMode::AES_128_ECB, true, false>,
            AESEncryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<DecryptImpl, EncryptionMode::AES_128_ECB, false, false>,
            AESDecryptName>>();

    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<EncryptImpl, EncryptionMode::AES_128_GCM, true, false, 5>,
            AESEncryptName>>();
    factory.register_function<FunctionEncryptionAndDecrypt<
            EncryptionAndDecryptMultiImpl<DecryptImpl, EncryptionMode::AES_128_GCM, false, false,
                                          5>,
            AESDecryptName>>();
}

} // namespace doris::vectorized
