#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct App
{
    std::string desktop_id;
    std::string desktop_file;

    std::string name;
    std::string generic_name;
    std::string comment;

    std::string exec;
    std::string try_exec;
    std::string icon;

    bool terminal = false;
    bool visible = true;

    std::vector<std::string> categories;
    std::vector<std::string> keywords;
};

struct AppGroup
{
    std::string name;
    std::vector<std::size_t> app_indices;
};

class Database
{
public:
    bool load(const std::string& path);

    const std::vector<App>& apps() const;
    const std::vector<AppGroup>& groups() const;

private:
    std::vector<App> apps_;
    std::vector<AppGroup> groups_;

    void build_groups();
};
