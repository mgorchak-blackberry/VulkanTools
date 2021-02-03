/*
 * Copyright (c) 2020 Valve Corporation
 * Copyright (c) 2020 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Authors:
 * - Christophe Riccio <christophe@lunarg.com>
 */

#pragma once

#include "header.h"
#include "setting_data.h"

#include <string>
#include <memory>

struct SettingMeta : public Header {
    SettingMeta(const std::string& key, const SettingType type);

    virtual bool operator==(const SettingMeta& setting_meta) const;

    bool operator!=(const SettingMeta& setting_meta) const { return !(*this == setting_meta); }

    const std::string key;
    const SettingType type;
};

struct SettingMetaString : public SettingMeta {
    SettingMetaString(const std::string& key) : SettingMeta(key, SETTING_STRING) {}

    std::string default_value;
};

struct SettingMetaInt : public SettingMeta {
    SettingMetaInt(const std::string& key) : SettingMeta(key, SETTING_INT), default_value(0) {}

    int default_value;
};

struct SettingMetaBool : public SettingMeta {
    SettingMetaBool(const std::string& key) : SettingMeta(key, SETTING_BOOL), default_value(false) {}

    bool default_value;
};

struct SettingMetaBoolNumeric : public SettingMeta {
    SettingMetaBoolNumeric(const std::string& key) : SettingMeta(key, SETTING_BOOL_NUMERIC_DEPRECATED), default_value(false) {}

    bool default_value;
};

struct SettingMetaIntRange : public SettingMeta {
    SettingMetaIntRange(const std::string& key) : SettingMeta(key, SETTING_INT_RANGE), default_min_value(0), default_max_value(0) {}

    int default_min_value;
    int default_max_value;
};

struct SettingMetaFilesystem : public SettingMeta {
    SettingMetaFilesystem(const std::string& key, const SettingType& setting_type) : SettingMeta(key, setting_type) {}

    std::string default_value;
    std::string filter;
};

struct SettingMetaFileLoad : public SettingMetaFilesystem {
    SettingMetaFileLoad(const std::string& key) : SettingMetaFilesystem(key, SETTING_LOAD_FILE) {}
};

struct SettingMetaFileSave : public SettingMetaFilesystem {
    SettingMetaFileSave(const std::string& key) : SettingMetaFilesystem(key, SETTING_SAVE_FILE) {}
};

struct SettingMetaFolderSave : public SettingMetaFilesystem {
    SettingMetaFolderSave(const std::string& key) : SettingMetaFilesystem(key, SETTING_SAVE_FOLDER) {}
};

struct SettingEnumValue : public Header {
    std::string key;
};

bool operator==(const SettingEnumValue& a, const SettingEnumValue& b);
inline bool operator!=(const SettingEnumValue& a, const SettingEnumValue& b) { return !(a == b); }

struct SettingMetaEnumeration : public SettingMeta {
    SettingMetaEnumeration(const std::string& key, const SettingType& setting_type) : SettingMeta(key, setting_type) {}

    virtual bool operator==(const SettingMeta& setting_meta) const {
        if (SettingMeta::operator!=(setting_meta)) return false;

        const SettingMetaEnumeration& setting_meta_enum = static_cast<const SettingMetaEnumeration&>(setting_meta);

        if (this->enum_values.size() != setting_meta_enum.enum_values.size()) return false;
        if (this->enum_values != setting_meta_enum.enum_values) return false;

        return true;
    }

    std::vector<SettingEnumValue> enum_values;
};

struct SettingMetaEnum : public SettingMetaEnumeration {
    SettingMetaEnum(const std::string& key) : SettingMetaEnumeration(key, SETTING_ENUM) {}

    std::string default_value;
};

struct SettingMetaFlags : public SettingMetaEnumeration {
    SettingMetaFlags(const std::string& key) : SettingMetaEnumeration(key, SETTING_FLAGS) {}

    std::vector<std::string> default_value;
};

struct SettingMetaVUIDFilter : public SettingMeta {
    SettingMetaVUIDFilter(const std::string& key) : SettingMeta(key, SETTING_VUID_FILTER) {}

    std::vector<std::string> list;
    std::vector<std::string> default_value;
};

class SettingMetaSet {
   public:
    SettingMeta& Create(const std::string& key, SettingType type);
    SettingMeta* Get(const char* key);
    const SettingMeta* Get(const char* key) const;
    bool Empty() const { return this->data.empty(); }

    std::vector<std::shared_ptr<SettingMeta> > data;
};