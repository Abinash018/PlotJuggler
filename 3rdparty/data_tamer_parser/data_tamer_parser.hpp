#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <optional>
#include <vector>
#include <variant>
#include <unordered_map>
#include <limits>
#include <functional>


namespace DataTamerParser {

constexpr int SCHEMA_VERSION = 3;

enum class BasicType
{
  BOOL,
  CHAR,
  INT8,
  UINT8,

  INT16,
  UINT16,

  INT32,
  UINT32,

  INT64,
  UINT64,

  FLOAT32,
  FLOAT64,
  OTHER
};

constexpr size_t TypesCount = 13;

using VarNumber = std::variant<
    bool, char,
    int8_t, uint8_t,
    int16_t, uint16_t,
    int32_t, uint32_t,
    int64_t, uint64_t,
    float, double >;

VarNumber DeserializeToVarNumber(BasicType type, const void* src);

/**
 * @brief DataTamer uses a simple "flat" schema of key/value pairs (each pair is a "field").
 */
struct Schema
{
  struct Field
  {
    std::string name;
    BasicType type = BasicType::OTHER;
    bool is_vector = 0;
    uint16_t array_size = 0;
    std::string custom_type_name;

    bool operator==(const Field &other) const;

    bool operator!=(const Field &other) const {
      return !(*this == other);
    }
  };
  std::vector<Field> fields;
  uint64_t hash = 0;
  std::string channel_name;
};

Schema BuilSchemaFromText(const std::string& txt);

struct BufferSpan
{
  const uint8_t* data = nullptr;
  size_t size = 0;

  void trimFront(size_t n) {
    data += n;
    size -= n;
  }
};

struct SnapshotView {
  /// Unique identifier of the schema
  size_t schema_hash;

  /// snapshot timestamp
  uint64_t timestamp;

  /// Vector that tell us if a field of the schema is
  /// active or not. It is basically an optimized vector
  /// of bools, where each byte contains 8 boolean flags.
  BufferSpan active_mask;

  /// serialized data containing all the values, ordered as in the schema
  BufferSpan payload;
};

bool GetBit(BufferSpan mask, size_t index);


constexpr auto NullCustomCallback = [](const std::string&, const BufferSpan, const std::string&){};
// Callback must be a std::function or lambda with signature:
//
// void(const std::string& name_field, const VarNumber& value)
//
// void(const std::string& name_field, const BufferSpan payload, const std::string& type_name)
//
template <typename NumberCallback, typename CustomCallback = decltype(NullCustomCallback)>
bool ParseSnapshot(const Schema& schema,
                   SnapshotView snapshot,
                   const NumberCallback& callback_number,
                   const CustomCallback& callback_custom = NullCustomCallback);

//---------------------------------------------------------
//---------------------------------------------------------
//---------------------------------------------------------

template<typename T> inline
    T Deserialize(BufferSpan& buffer)
{
  T var;
  const auto N = sizeof(T);
  std::memcpy(&var, buffer.data, N);
  buffer.data += N;
  if( N > buffer.size)
  {
    throw std::runtime_error("Buffer overflow");
  }
  buffer.size -= N;
  return var;
}

inline VarNumber DeserializeToVarNumber(BasicType type,
                                        BufferSpan& src)
{
  switch(type)
  {
    case BasicType::BOOL: return Deserialize<bool>(src);
    case BasicType::CHAR: return Deserialize<char>(src);

    case BasicType::INT8: return Deserialize<int8_t>(src);
    case BasicType::UINT8: return Deserialize<uint8_t>(src);

    case BasicType::INT16: return Deserialize<int16_t>(src);
    case BasicType::UINT16: return Deserialize<uint16_t>(src);

    case BasicType::INT32: return Deserialize<int32_t>(src);
    case BasicType::UINT32: return Deserialize<uint32_t>(src);

    case BasicType::INT64: return Deserialize<int64_t>(src);
    case BasicType::UINT64: return Deserialize<uint64_t>(src);

    case BasicType::FLOAT32: return Deserialize<float>(src);
    case BasicType::FLOAT64: return Deserialize<double>(src);

    case BasicType::OTHER:
      return double(std::numeric_limits<double>::quiet_NaN());
  }
  return {};
}

inline bool GetBit(BufferSpan mask, size_t index)
{
  const uint8_t& byte = mask.data[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

[[nodiscard]] inline uint64_t AddFieldToHash(const Schema::Field &field, uint64_t hash)
{
  const std::hash<std::string> str_hasher;
  const std::hash<BasicType> type_hasher;
  const std::hash<bool> bool_hasher;
  const std::hash<uint16_t> uint_hasher;

  auto combine = [&hash](const auto& hasher, const auto& val)
  {
    hash ^= hasher(val) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  };

  combine(str_hasher, field.name);
  combine(type_hasher, field.type);
  if(field.type == BasicType::OTHER)
  {
    combine(str_hasher, field.custom_type_name);
  }
  combine(bool_hasher, field.is_vector);
  combine(uint_hasher, field.array_size);
  return hash;
}

bool Schema::Field::operator==(const Field &other) const
{
  return is_vector == other.is_vector &&
         type == other.type &&
         array_size == other.array_size &&
         name == other.name;
}

inline Schema BuilSchemaFromText(const std::string& txt)
{
  auto trimString = [](std::string& str)
  {
    while(str.back() == ' ' || str.back() == '\r') {
      str.pop_back();
    }
    while(str.front() == ' ' || str.front() == '\r') {
      str.erase(0, 1);
    }
  };

  std::istringstream ss(txt);
  std::string line;
  Schema schema;
  uint64_t declared_schema = 0;

  while (std::getline(ss, line))
  {
    trimString(line);
    if(line.empty())
    {
      continue;
    }
    if(line == "---------")
    {
      // we are not interested to this section of the schema
      break;
    }

    // a single space is expected
    auto space_pos = line.find(' ');
    if(space_pos == std::string::npos)
    {
      throw std::runtime_error("Unexpected line: " + line);
    }
    std::string str_left = line.substr(0, space_pos);
    std::string str_right = line.substr(space_pos+1, line.size() - (space_pos+1));
    trimString(str_left);
    trimString(str_right);

    const std::string* str_type = &str_left;
    const std::string* str_name = &str_right;

    if(str_left == "__version__:")
    {
      // check compatibility
      if(std::stoi(str_right) != SCHEMA_VERSION)
      {
        throw std::runtime_error("Wrong SCHEMA_VERSION");
      }
      continue;
    }
    if(str_left == "__hash__:")
    {
      // check compatibility
      declared_schema = std::stoul(str_right);
      continue;
    }

    if(str_left == "__channel_name__:")
    {
      // check compatibility
      schema.channel_name = str_right;
      schema.hash = std::hash<std::string>()(schema.channel_name);
      continue;
    }

    Schema::Field field;

    static const std::array<std::string, TypesCount> kNamesNew = {
      "bool", "char",
      "int8", "uint8",
      "int16", "uint16",
      "int32", "uint32",
      "int64", "uint64",
      "float32", "float64",
      "other"
    };
    // backcompatibility to old format
    static const std::array<std::string, TypesCount> kNamesOld = {
      "BOOL", "CHAR",
      "INT8", "UINT8",
      "INT16", "UINT16",
      "INT32", "UINT32",
      "INT64", "UINT64",
      "FLOAT", "DOUBLE",
      "OTHER"
    };

    for(size_t i=0; i<TypesCount; i++)
    {
      if(str_left.find(kNamesNew[i]) == 0)
      {
        field.type = static_cast<BasicType>(i);
        break;
      }
      if(str_right.find(kNamesOld[i]) == 0)
      {
        field.type = static_cast<BasicType>(i);
        std::swap(str_type, str_name);
        break;
      }
    }

    if(field.type == BasicType::OTHER)
    {
      field.custom_type_name = *str_type;
    }

    auto offset = str_type->find_first_of(" [");
    if(offset != std::string::npos && str_type->at(offset)=='[')
    {
      field.is_vector = true;
      auto pos = str_type->find(']', offset);
      if(pos != offset+1)
      {
        // get number
        std::string number_string = line.substr(offset+1, pos - offset - 1);
        field.array_size = static_cast<uint16_t>(std::stoi(number_string));
      }
    }

    field.name = *str_name;
    trimString(field.name);

            // update the hash
    schema.hash = AddFieldToHash(field, schema.hash);

    schema.fields.push_back(field);
  }
  if(declared_schema != 0 && declared_schema != schema.hash)
  {
    throw std::runtime_error("Error in hash calculation");
  }
  return schema;
}

template <typename NumberCallback, typename CustomCallback> inline
    bool ParseSnapshot(const Schema& schema,
                  SnapshotView snapshot,
                  const NumberCallback& callback_number,
                  const CustomCallback& callback_custom)
{
  if(schema.hash != snapshot.schema_hash)
  {
    return false;
  }
  BufferSpan buffer = snapshot.payload;

  for(size_t i=0; i<schema.fields.size(); i++)
  {
    const auto& field = schema.fields[i];
    if(GetBit(snapshot.active_mask, i))
    {
      if(!field.is_vector)
      {
        // regular field, not vector/array
        if(field.type == BasicType::OTHER)
        {
          callback_custom(field.name, snapshot.payload, field.custom_type_name);
        }
        else {
          const auto var = DeserializeToVarNumber(field.type, buffer);
          callback_number(field.name, var);
        }
      }
      else
      {
        uint32_t vect_size = field.array_size;
        if(field.array_size == 0) // dynamic vector
        {
          vect_size = Deserialize<uint32_t>(buffer);
        }
        for(size_t a=0; a<vect_size; a++)
        {
          thread_local std::string tmp_name;
          tmp_name = field.name + "[" + std::to_string(a) + "]";

          if(field.type == BasicType::OTHER)
          {
            callback_custom(tmp_name, snapshot.payload, field.custom_type_name);
          }
          else {
            const auto var = DeserializeToVarNumber(field.type, buffer);
            callback_number(tmp_name, var);
          }
        }
      }
    }
  }
  return true;
}

}  // namespace DataTamer
