#include "db.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>


namespace
{

constexpr char MAGIC[] = "LATTICE1";
constexpr std::uint32_t VERSION = 2;


// ============================================================
// Binary DB reading helpers
// ============================================================

bool read_string_raw(std::ifstream& file, std::string& value)
{
    std::uint32_t length = 0;

    if (!file.read(
            reinterpret_cast<char*>(&length),
            sizeof(length)))
    {
        return false;
    }

    // Prevent corrupt DB from causing excessive allocation.
    if (length > 1024 * 1024)
        return false;

    value.resize(length);

    if (length == 0)
        return true;

    return static_cast<bool>(
        file.read(
            value.data(),
            static_cast<std::streamsize>(length))
    );
}


bool read_string_vector_raw(
    std::ifstream& file,
    std::vector<std::string>& values)
{
    std::uint32_t count = 0;

    if (!file.read(
            reinterpret_cast<char*>(&count),
            sizeof(count)))
    {
        return false;
    }

    // Sanity limit for corrupted DB files.
    if (count > 10000)
        return false;

    values.clear();
    values.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::string value;

        if (!read_string_raw(file, value))
            return false;

        values.push_back(std::move(value));
    }

    return true;
}


bool read_bool_raw(std::ifstream& file, bool& value)
{
    std::uint8_t byte = 0;

    if (!file.read(
            reinterpret_cast<char*>(&byte),
            sizeof(byte)))
    {
        return false;
    }

    value = (byte != 0);
    return true;
}


// ============================================================
// Group reading
// ============================================================
//
// Each group is:
//
//     name          string
//     member_count  u32
//     member_index  u32 * member_count
//
bool read_group_raw(std::ifstream& file, AppGroup& group)
{
    if (!read_string_raw(file, group.name))
        return false;

    std::uint32_t member_count = 0;

    if (!file.read(
            reinterpret_cast<char*>(&member_count),
            sizeof(member_count)))
    {
        return false;
    }

    if (member_count > 100000)
        return false;

    group.app_indices.clear();
    group.app_indices.reserve(member_count);

    for (std::uint32_t i = 0; i < member_count; ++i)
    {
        std::uint32_t index = 0;

        if (!file.read(
                reinterpret_cast<char*>(&index),
                sizeof(index)))
        {
            return false;
        }

        group.app_indices.push_back(
            static_cast<std::size_t>(index)
        );
    }

    return true;
}

} // namespace


// ============================================================
// Database::load
// ============================================================
//
// Reads the full v2 database:
//
//     magic[8]      "LATTICE1"
//     version u32   2
//     app_count u32
//       App[]
//     group_count u32
//       AppGroup[]
//
// The daemon writes the groups. The UI just reads them.
// Nothing is computed here.
//
bool Database::load(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
        return false;


    // --------------------------------------------------------
    // Magic
    // --------------------------------------------------------

    char magic[sizeof(MAGIC) - 1];

    if (!file.read(magic, sizeof(magic)))
        return false;

    if (!std::equal(
            std::begin(magic),
            std::end(magic),
            MAGIC))
    {
        return false;
    }


    // --------------------------------------------------------
    // Version
    // --------------------------------------------------------

    std::uint32_t version = 0;

    if (!file.read(
            reinterpret_cast<char*>(&version),
            sizeof(version)))
    {
        return false;
    }

    if (version != VERSION)
        return false;


    // --------------------------------------------------------
    // Application count
    // --------------------------------------------------------

    std::uint32_t app_count = 0;

    if (!file.read(
            reinterpret_cast<char*>(&app_count),
            sizeof(app_count)))
    {
        return false;
    }

    if (app_count > 100000)
        return false;


    // --------------------------------------------------------
    // Read all applications
    // --------------------------------------------------------

    std::vector<App> new_apps;
    new_apps.reserve(app_count);

    for (std::uint32_t i = 0; i < app_count; ++i)
    {
        App app;

        if (!read_string_raw(file, app.desktop_id))   return false;
        if (!read_string_raw(file, app.desktop_file)) return false;
        if (!read_string_raw(file, app.name))         return false;
        if (!read_string_raw(file, app.generic_name)) return false;
        if (!read_string_raw(file, app.comment))      return false;
        if (!read_string_raw(file, app.exec))         return false;
        if (!read_string_raw(file, app.try_exec))     return false;
        if (!read_string_raw(file, app.icon))         return false;

        if (!read_bool_raw(file, app.terminal))       return false;
        if (!read_bool_raw(file, app.visible))        return false;

        if (!read_string_vector_raw(file, app.categories)) return false;
        if (!read_string_vector_raw(file, app.keywords))   return false;

        new_apps.push_back(std::move(app));
    }


    // --------------------------------------------------------
    // Group count
    // --------------------------------------------------------

    std::uint32_t group_count = 0;

    if (!file.read(
            reinterpret_cast<char*>(&group_count),
            sizeof(group_count)))
    {
        return false;
    }

    if (group_count > 100000)
        return false;


    // --------------------------------------------------------
    // Read all groups
    // --------------------------------------------------------

    std::vector<AppGroup> new_groups;
    new_groups.reserve(group_count);

    for (std::uint32_t i = 0; i < group_count; ++i)
    {
        AppGroup group;

        if (!read_group_raw(file, group))
            return false;

        new_groups.push_back(std::move(group));
    }


    // --------------------------------------------------------
    // Replace current state only after the entire DB loaded.
    // --------------------------------------------------------

    apps_   = std::move(new_apps);
    groups_ = std::move(new_groups);

    return true;
}


// ============================================================
// Accessors
// ============================================================

const std::vector<App>& Database::apps() const
{
    return apps_;
}


const std::vector<AppGroup>& Database::groups() const
{
    return groups_;
}
