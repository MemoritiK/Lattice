#include <garcon/garcon.h>
#include <glib.h>
#include <glib-unix.h>
#include "db.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
namespace fs = std::filesystem;


/*
 * ============================================================
 * Daemon
 * ============================================================
 */

class Daemon
{
public:
    ~Daemon();

    bool initialize();
    void run();
    void shutdown();
    void rescan();


private:
    GarconMenu* menu = nullptr;
    GMainLoop*  loop = nullptr;

    std::vector<App>      apps;
    std::vector<AppGroup> groups;

    std::string xdg_current_desktop;
    std::string xdg_session_desktop;

    static constexpr const char* DB_DIRECTORY =
        "/tmp/lattice";

    static constexpr const char* DB_FILE =
        "/tmp/lattice/apps.db";

    static constexpr const char* DB_TEMP_FILE =
        "/tmp/lattice/apps.db.tmp";


    /*
     * Initialization
     */
    bool initialize_environment();
    bool initialize_garcon();


    /*
     * Application collection
     */
    void rebuild_database();

    void collect_menu(GarconMenu* current_menu);

    void add_app(GarconMenuItem* item);

    bool desktop_environment_visible(
        GarconMenuItem* item);

    std::unordered_set<std::string> seen_desktop_files;
    std::unordered_set<std::string> seen_desktop_ids;

    /*
     * Grouping
     */
    void build_groups();


    /*
     * Database
     */
    bool write_database();

    bool write_string(
        std::ofstream& out,
        const std::string& value);

    bool write_bool(
        std::ofstream& out,
        bool value);

    bool write_string_vector(
        std::ofstream& out,
        const std::vector<std::string>& values);

    bool write_group_vector(
        std::ofstream& out,
        const std::vector<AppGroup>& values);


    /*
     * Debugging
     */
    void print_environment() const;
    void print_database() const;


    /*
     * Garcon callbacks
     */
    static void item_callback(
        gpointer key,
        gpointer value,
        gpointer user_data);

    static void on_reload_required(
        GarconMenu* menu,
        Daemon* self);
};


/*
 * ============================================================
 * Destructor
 * ============================================================
 */

Daemon::~Daemon()
{
    if (loop != nullptr)
    {
        if (g_main_loop_is_running(loop))
            g_main_loop_quit(loop);

        g_main_loop_unref(loop);
        loop = nullptr;
    }

    if (menu != nullptr)
    {
        g_object_unref(menu);
        menu = nullptr;
    }
}



/*
 * ============================================================
 * Environment
 * ============================================================
 */

bool Daemon::initialize_environment()
{
    const char* current =
        std::getenv("XDG_CURRENT_DESKTOP");

    const char* session =
        std::getenv("XDG_SESSION_DESKTOP");


    if (current != nullptr && *current != '\0')
        xdg_current_desktop = current;

    if (session != nullptr && *session != '\0')
        xdg_session_desktop = session;


    if (xdg_current_desktop.empty() &&
        !xdg_session_desktop.empty())
    {
        xdg_current_desktop = xdg_session_desktop;
    }


    print_environment();

    return true;
}


/*
 * ============================================================
 * Debug environment
 * ============================================================
 */

void Daemon::print_environment() const
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " Lattice environment\n";
    std::cout << "========================================\n";

    std::cout
        << "XDG_CURRENT_DESKTOP : "
        << (xdg_current_desktop.empty() ? "(not set)" : xdg_current_desktop)
        << '\n';

    std::cout
        << "XDG_SESSION_DESKTOP : "
        << (xdg_session_desktop.empty() ? "(not set)" : xdg_session_desktop)
        << '\n';

    std::cout
        << "Environment used    : "
        << (xdg_current_desktop.empty() ? "(none)" : xdg_current_desktop)
        << '\n';

    std::cout << "========================================\n";
}


/*
 * ============================================================
 * Garcon initialization
 * ============================================================
 */

bool Daemon::initialize_garcon()
{
    std::cout << "\n";
    std::cout << "Initializing Garcon...\n";

    menu = garcon_menu_new_applications();

    if (menu == nullptr)
    {
        std::cerr << "Failed to create applications menu.\n";
        return false;
    }

    GError* error = nullptr;

    if (!garcon_menu_load(menu, nullptr, &error))
    {
        std::cerr << "Failed to load applications menu";

        if (error != nullptr)
        {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }

        std::cerr << '\n';
        return false;
    }

    g_signal_connect(
        menu,
        "reload-required",
        G_CALLBACK(Daemon::on_reload_required),
        this
    );

    std::cout << "Garcon initialized.\n";
    return true;
}


/*
 * ============================================================
 * Main initialization
 * ============================================================
 */

bool Daemon::initialize()
{
    if (!initialize_environment())
        return false;

    if (!initialize_garcon())
        return false;

    rebuild_database();

    return true;
}


void Daemon::rescan()
{
    std::cout << "[Lattice] Rescan requested.\n";

    GError* error = nullptr;

    if (!garcon_menu_load(menu, nullptr, &error))
    {
        std::cerr << "[Lattice] Rescan failed";

        if (error != nullptr)
        {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }

        std::cerr << '\n';
        return;
    }

    rebuild_database();
}

/*
 * ============================================================
 * Desktop environment filtering
 * ============================================================
 */

bool Daemon::desktop_environment_visible(GarconMenuItem* item)
{
    if (item == nullptr)
        return false;

    GFile* file = garcon_menu_item_get_file(item);

    if (file == nullptr)
        return false;

    gchar* path = g_file_get_path(file);

    if (path == nullptr)
        return false;

    GKeyFile* keyfile = g_key_file_new();
    GError* error = nullptr;

    bool loaded = g_key_file_load_from_file(
        keyfile, path, G_KEY_FILE_NONE, &error);

    g_free(path);

    if (!loaded)
    {
        if (error != nullptr)
            g_error_free(error);

        g_key_file_free(keyfile);
        return false;
    }

    // ---------------- OnlyShowIn ----------------
    gsize only_count = 0;
    gchar** only_show_in = g_key_file_get_string_list(
        keyfile, "Desktop Entry", "OnlyShowIn", &only_count, nullptr);

    if (only_show_in != nullptr)
    {
        bool match = false;

        for (gsize i = 0; i < only_count; ++i)
        {
            if (only_show_in[i] != nullptr &&
                xdg_current_desktop == only_show_in[i])
            {
                match = true;
                break;
            }
        }

        g_strfreev(only_show_in);

        if (!match)
        {
            g_key_file_free(keyfile);
            return false;
        }
    }

    // ---------------- NotShowIn ----------------
    gsize not_count = 0;
    gchar** not_show_in = g_key_file_get_string_list(
        keyfile, "Desktop Entry", "NotShowIn", &not_count, nullptr);

    if (not_show_in != nullptr)
    {
        for (gsize i = 0; i < not_count; ++i)
        {
            if (not_show_in[i] != nullptr &&
                xdg_current_desktop == not_show_in[i])
            {
                g_strfreev(not_show_in);
                g_key_file_free(keyfile);
                return false;
            }
        }

        g_strfreev(not_show_in);
    }

    g_key_file_free(keyfile);
    return true;
}


/*
 * ============================================================
 * Rebuild database
 * ============================================================
 */

void Daemon::rebuild_database()
{
    std::cout << "\n";
    std::cout << "[Lattice] Rebuilding application database...\n";


    apps.clear();
    groups.clear();
    
    seen_desktop_files.clear();
    seen_desktop_ids.clear();

    collect_menu(menu);

    std::cout
        << "[Lattice] Visible applications: "
        << apps.size()
        << '\n';

    build_groups();

    std::cout
        << "[Lattice] Groups: "
        << groups.size()
        << '\n';

    if (!write_database())
    {
        std::cerr << "[Lattice] Failed to write application database.\n";
        return;
    }

    std::cout << "[Lattice] Database updated: " << DB_FILE << '\n';

    print_database();
}


/*
 * ============================================================
 * Walk Garcon menu
 * ============================================================
 */

void Daemon::collect_menu(GarconMenu* current_menu)
{
    if (current_menu == nullptr)
        return;

    GarconMenuItemPool* pool =
        garcon_menu_get_item_pool(current_menu);

    if (pool != nullptr)
    {
        garcon_menu_item_pool_foreach(
            pool, Daemon::item_callback, this);
    }

    GList* menus = garcon_menu_get_menus(current_menu);

    for (GList* l = menus; l != nullptr; l = l->next)
    {
        GarconMenu* submenu = GARCON_MENU(l->data);
        collect_menu(submenu);
    }

    g_list_free(menus);
}


void Daemon::item_callback(
    gpointer /*key*/,
    gpointer value,
    gpointer user_data)
{
    if (value == nullptr || user_data == nullptr)
        return;

    GarconMenuItem* item = GARCON_MENU_ITEM(value);
    auto* daemon = static_cast<Daemon*>(user_data);

    daemon->add_app(item);
}


/*
 * ============================================================
 * Convert Garcon item -> App
 * ============================================================
 */

void Daemon::add_app(GarconMenuItem* item)
{
    if (item == nullptr)
        return;

    if (!garcon_menu_element_get_visible(GARCON_MENU_ELEMENT(item)))
        return;

    if (!desktop_environment_visible(item))
        return;

    App app;

    const gchar* desktop_id   = garcon_menu_item_get_desktop_id(item);
    const gchar* name         = garcon_menu_item_get_name(item);
    const gchar* generic_name = garcon_menu_item_get_generic_name(item);
    const gchar* comment      = garcon_menu_item_get_comment(item);
    const gchar* exec         = garcon_menu_item_get_command(item);
    const gchar* try_exec     = garcon_menu_item_get_try_exec(item);
    const gchar* icon         = garcon_menu_item_get_icon_name(item);

    if (desktop_id)   app.desktop_id   = desktop_id;
    if (name)         app.name         = name;
    if (generic_name) app.generic_name = generic_name;
    if (comment)      app.comment      = comment;
    if (exec)         app.exec         = exec;
    if (try_exec)     app.try_exec     = try_exec;
    if (icon)         app.icon         = icon;

    GFile* file = garcon_menu_item_get_file(item);

    if (file != nullptr)
    {
        gchar* path = g_file_get_path(file);

        if (path != nullptr)
        {
            app.desktop_file = path;
            g_free(path);
        }
    }

    app.terminal = garcon_menu_item_requires_terminal(item);
    app.visible = true;

    GList* categories = garcon_menu_item_get_categories(item);

    for (GList* l = categories; l != nullptr; l = l->next)
    {
        if (l->data != nullptr)
        {
            app.categories.emplace_back(
                static_cast<const char*>(l->data));
        }
    }

    GList* keywords = garcon_menu_item_get_keywords(item);

    for (GList* l = keywords; l != nullptr; l = l->next)
    {
        if (l->data != nullptr)
        {
            app.keywords.emplace_back(
                static_cast<const char*>(l->data));
        }
    }

    // Prevent duplicate applications.
    if (!app.desktop_file.empty()) {
        if (!seen_desktop_files.insert(app.desktop_file).second) {
            std::cout << "[Lattice] Skipping duplicate desktop file: "
                      << app.desktop_file << "\n";
            return;
        }
    }
    
    if (!app.desktop_id.empty()) {
        if (!seen_desktop_ids.insert(app.desktop_id).second) {
            std::cout << "[Lattice] Skipping duplicate desktop ID: "
                      << app.desktop_id << "\n";
            return;
        }
    }
    
    apps.push_back(std::move(app));
}


/*
 * ============================================================
 * Grouping
 * ============================================================
 *
 * Prefix-words on App::name only. Whole words, from the
 * beginning of the name. No substrings, no suffix list,
 * no generic_name.
 *
 *   Foot / Foot Server / Foot Client  ->  "Foot"
 *   Qt V4L2 / Qt V4L2 Base / ...      ->  "Qt V4L2"
 *   LibreOffice Writer / Calc / ...   ->  "LibreOffice"
 *
 * Singleton apps become a group named after themselves.
 *
 * Deterministic output:
 *   - groups sorted alphabetically by group name
 *   - members sorted alphabetically by app.name
 */
void Daemon::build_groups()
{
    groups.clear();

    if (apps.empty())
        return;

    // ---- tokenise names ----
    std::vector<std::vector<std::string>> words(apps.size());

    for (std::size_t i = 0; i < apps.size(); ++i)
    {
        const std::string& s = apps[i].name;
        std::vector<std::string> w;

        std::size_t p = 0;

        while (p < s.size())
        {
            while (p < s.size() &&
                   std::isspace(static_cast<unsigned char>(s[p])))
                ++p;

            std::size_t start = p;

            while (p < s.size() &&
                   !std::isspace(static_cast<unsigned char>(s[p])))
                ++p;

            if (p > start)
                w.emplace_back(s.substr(start, p - start));
        }

        words[i] = std::move(w);
    }

    // ---- count distinct apps per prefix ----
    std::unordered_map<std::string, std::size_t> prefix_count;

    for (std::size_t i = 0; i < apps.size(); ++i)
    {
        if (words[i].empty())
            continue;

        std::string acc;
        std::unordered_set<std::string> local;

        for (std::size_t k = 0; k < words[i].size(); ++k)
        {
            if (k)
                acc += ' ';

            acc += words[i][k];

            if (local.insert(acc).second)
                prefix_count[acc] += 1;
        }
    }

    // ---- pick longest shared prefix per app ----
    std::unordered_map<std::string, std::vector<std::size_t>> bucket;
    std::vector<std::string> order;

    for (std::size_t i = 0; i < apps.size(); ++i)
    {
        std::string chosen;

        if (!words[i].empty())
        {
            std::string acc;
            std::vector<std::string> candidates;
            candidates.reserve(words[i].size());

            for (std::size_t k = 0; k < words[i].size(); ++k)
            {
                if (k)
                    acc += ' ';

                acc += words[i][k];
                candidates.push_back(acc);
            }

            for (auto it = candidates.rbegin();
                 it != candidates.rend();
                 ++it)
            {
                auto found = prefix_count.find(*it);

                if (found != prefix_count.end() &&
                    found->second >= 2)
                {
                    chosen = *it;
                    break;
                }
            }
        }

        if (chosen.empty())
            chosen = apps[i].name;

        auto [it, inserted] = bucket.try_emplace(chosen);

        if (inserted)
            order.push_back(chosen);

        it->second.push_back(i);
    }

    // ---- materialise ----
    groups.reserve(order.size());

    for (auto& gname : order)
    {
        AppGroup g;
        g.name = gname;

        auto it = bucket.find(gname);

        if (it != bucket.end())
            g.app_indices = std::move(it->second);

        std::sort(
            g.app_indices.begin(),
            g.app_indices.end(),
            [&](std::size_t a, std::size_t b) {
                return apps[a].name < apps[b].name;
            });

        groups.push_back(std::move(g));
    }

    std::sort(
        groups.begin(),
        groups.end(),
        [](const AppGroup& a, const AppGroup& b) {
            return a.name < b.name;
        });
}


/*
 * ============================================================
 * Write helpers
 * ============================================================
 */

bool Daemon::write_string(
    std::ofstream& out,
    const std::string& value)
{
    uint32_t length = static_cast<uint32_t>(value.size());

    out.write(
        reinterpret_cast<const char*>(&length),
        sizeof(length));

    if (length > 0)
        out.write(value.data(), length);

    return out.good();
}


bool Daemon::write_bool(
    std::ofstream& out,
    bool value)
{
    uint8_t byte = value ? 1 : 0;

    out.write(
        reinterpret_cast<const char*>(&byte),
        sizeof(byte));

    return out.good();
}


bool Daemon::write_string_vector(
    std::ofstream& out,
    const std::vector<std::string>& values)
{
    uint32_t count = static_cast<uint32_t>(values.size());

    out.write(
        reinterpret_cast<const char*>(&count),
        sizeof(count));

    for (const auto& value : values)
    {
        if (!write_string(out, value))
            return false;
    }

    return out.good();
}


bool Daemon::write_group_vector(
    std::ofstream& out,
    const std::vector<AppGroup>& values)
{
    uint32_t count = static_cast<uint32_t>(values.size());

    out.write(
        reinterpret_cast<const char*>(&count),
        sizeof(count));

    for (const auto& g : values)
    {
        if (!write_string(out, g.name))
            return false;

        uint32_t members =
            static_cast<uint32_t>(g.app_indices.size());

        out.write(
            reinterpret_cast<const char*>(&members),
            sizeof(members));

        for (std::size_t idx : g.app_indices)
        {
            uint32_t v = static_cast<uint32_t>(idx);

            out.write(
                reinterpret_cast<const char*>(&v),
                sizeof(v));
        }

        if (!out.good())
            return false;
    }

    return out.good();
}


/*
 * ============================================================
 * Write application database
 * ============================================================
 *
 * Layout (unchanged header, group block appended):
 *
 *   magic[8]      "LATTICE1"
 *   version u32   1
 *   app_count u32
 *     App[]
 *   group_count u32
 *     AppGroup[]
 *
 * A v1 reader that only knows about the app block will read
 * the apps correctly and stop. The group block is only read
 * by the newer reader.
 */

bool Daemon::write_database()
{
    std::error_code ec;

    fs::create_directories(DB_DIRECTORY, ec);

    if (ec)
    {
        std::cerr
            << "[DB] Failed to create directory "
            << DB_DIRECTORY
            << ": "
            << ec.message()
            << '\n';

        return false;
    }

    std::ofstream out(
        DB_TEMP_FILE,
        std::ios::binary | std::ios::trunc);

    if (!out)
    {
        std::cerr << "[DB] Failed to open temporary database.\n";
        return false;
    }

    const char magic[8] =
    {
        'L', 'A', 'T', 'T',
        'I', 'C', 'E', '1'
    };

    uint32_t version = 2;

    uint32_t app_count = static_cast<uint32_t>(apps.size());

    out.write(magic, sizeof(magic));
    out.write(
        reinterpret_cast<const char*>(&version),
        sizeof(version));
    out.write(
        reinterpret_cast<const char*>(&app_count),
        sizeof(app_count));

    for (const auto& app : apps)
    {
        if (!write_string(out, app.desktop_id))   return false;
        if (!write_string(out, app.desktop_file)) return false;
        if (!write_string(out, app.name))         return false;
        if (!write_string(out, app.generic_name)) return false;
        if (!write_string(out, app.comment))      return false;
        if (!write_string(out, app.exec))         return false;
        if (!write_string(out, app.try_exec))     return false;
        if (!write_string(out, app.icon))         return false;
        if (!write_bool  (out, app.terminal))     return false;
        if (!write_bool  (out, app.visible))      return false;
        if (!write_string_vector(out, app.categories)) return false;
        if (!write_string_vector(out, app.keywords))   return false;
    }

    if (!write_group_vector(out, groups))
        return false;

    out.flush();

    if (!out.good())
    {
        out.close();
        std::cerr << "[DB] Error while writing database.\n";
        return false;
    }

    out.close();

    fs::rename(DB_TEMP_FILE, DB_FILE, ec);

    if (ec)
    {
        std::error_code remove_ec;
        fs::remove(DB_FILE, remove_ec);

        ec.clear();
        fs::rename(DB_TEMP_FILE, DB_FILE, ec);

        if (ec)
        {
            std::cerr
                << "[DB] Failed to replace database: "
                << ec.message()
                << '\n';

            return false;
        }
    }

    return true;
}


/*
 * ============================================================
 * Debug database contents
 * ============================================================
 */

void Daemon::print_database() const
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " Visible applications\n";
    std::cout << "========================================\n";

    for (const auto& app : apps)
    {
        std::cout << "----------------------------------------\n";
        std::cout << "Name       : " << app.name << '\n';
        std::cout << "Desktop ID : " << app.desktop_id << '\n';
        std::cout << "Desktop    : " << app.desktop_file << '\n';
        std::cout << "Exec       : " << app.exec << '\n';
        std::cout << "Icon       : " << app.icon << '\n';
        std::cout << "Terminal   : " << (app.terminal ? "yes" : "no") << '\n';

        std::cout << "Categories : ";
        for (const auto& category : app.categories)
            std::cout << category << ' ';
        std::cout << '\n';

        std::cout << "Keywords   : ";
        for (const auto& keyword : app.keywords)
            std::cout << keyword << ' ';
        std::cout << '\n';

        std::cout << "Visible    : " << (app.visible ? "yes" : "no") << '\n';
    }

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " Groups\n";
    std::cout << "========================================\n";

    for (const auto& g : groups)
    {
        std::cout << "----------------------------------------\n";
        std::cout << g.name << "  (" << g.app_indices.size() << ")\n";

        for (std::size_t idx : g.app_indices)
        {
            if (idx < apps.size())
                std::cout << "    " << apps[idx].name << '\n';
        }
    }

    std::cout << "========================================\n";
}


/*
 * ============================================================
 * Garcon reload callback
 * ============================================================
 *
 * Kept inline as before. The crash you were seeing on reload
 * is a reentrancy issue in Garcon itself, but since you asked
 * not to change the flow, this stays as it was.
 */

void Daemon::on_reload_required(
    GarconMenu* /*menu*/,
    Daemon* self)
{
    std::cout << "\n";
    std::cout << "[Garcon] Application menu changed.\n";

    GError* error = nullptr;

    if (!garcon_menu_load(self->menu, nullptr, &error))
    {
        std::cerr << "[Garcon] Reload failed";

        if (error != nullptr)
        {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }

        std::cerr << '\n';
        return;
    }

    std::cout << "[Garcon] Reload successful.\n";

    self->rebuild_database();
}


/*
 * ============================================================
 * Main loop
 * ============================================================
 */

void Daemon::run()
{
    std::cout << "\n";
    std::cout << "Lattice daemon running...\n";
    std::cout << "Waiting for Garcon changes...\n";

    loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
}


void Daemon::shutdown()
{
    if (loop != nullptr && g_main_loop_is_running(loop))
        g_main_loop_quit(loop);
}


/*
 * ============================================================
 * Signal handling
 * ============================================================
 */

static Daemon* g_daemon = nullptr;

static gboolean on_unix_signal(gpointer /*user_data*/)
{
    if (g_daemon != nullptr)
        g_daemon->shutdown();

    return G_SOURCE_REMOVE;
}

static gboolean on_rescan_signal(gpointer)   // ← add here
{
    if (g_daemon != nullptr)
        g_daemon->rescan();

    return G_SOURCE_CONTINUE;
}

int main()
{
    const std::string pid_dir  = "/tmp/lattice";
    const std::string pid_path = "/tmp/lattice/daemon.pid";

    fs::create_directories(pid_dir);

    // Open + lock the PID file. Held for the process lifetime;
    // released and unlinked in ~PidFile.
    int pid_fd = ::open(
        pid_path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC,
        0644);

    if (pid_fd == -1)
    {
        std::cerr << "open " << pid_path << ": "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    if (::flock(pid_fd, LOCK_EX | LOCK_NB) == -1)
    {
        if (errno == EWOULDBLOCK)
            std::cerr << "Lattice daemon is already running.\n";
        else
            std::cerr << "flock: " << std::strerror(errno) << '\n';

        ::close(pid_fd);
        return 1;
    }

    if (::ftruncate(pid_fd, 0) == -1)
    {
        std::cerr << "ftruncate: " << std::strerror(errno) << '\n';
        ::flock(pid_fd, LOCK_UN);
        ::close(pid_fd);
        return 1;
    }

    std::string pid = std::to_string(::getpid()) + "\n";

    if (::write(pid_fd, pid.data(), pid.size()) == -1)
    {
        std::cerr << "write pid: " << std::strerror(errno) << '\n';
        ::flock(pid_fd, LOCK_UN);
        ::close(pid_fd);
        return 1;
    }

    Daemon daemon;
    g_daemon = &daemon;

    if (!daemon.initialize())
    {
        g_daemon = nullptr;
        ::flock(pid_fd, LOCK_UN);
        ::close(pid_fd);
        ::unlink(pid_path.c_str());
        return 1;
    }

    g_unix_signal_add(SIGINT,  on_unix_signal, nullptr);
    g_unix_signal_add(SIGTERM, on_unix_signal, nullptr);
    g_unix_signal_add(SIGUSR2, on_rescan_signal, nullptr);

    daemon.run();

    g_daemon = nullptr;

    ::flock(pid_fd, LOCK_UN);
    ::close(pid_fd);
    ::unlink(pid_path.c_str());

    return 0;
}
