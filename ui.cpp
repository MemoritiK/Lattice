#include "db.hpp"

#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <glib-unix.h>
#include <glib.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


namespace fs = std::filesystem;


namespace
{

// ===========================================================================
// Constants
// ===========================================================================

constexpr const char* LATTICE_DIR      = "/tmp/lattice";
constexpr const char* SESSION_FILE     = "/tmp/lattice/session";
constexpr const char* DB_PATH          = "/tmp/lattice/apps.db";
constexpr const char* UI_PID_FILE      = "/tmp/lattice/ui.pid";
constexpr const char* UI_LOCK_FILE     = "/tmp/lattice/ui.lock";
constexpr const char* DAEMON_PID_FILE  = "/tmp/lattice/daemon.pid";
constexpr const char* DEFAULT_TERMINAL = "foot";

constexpr int ICON_SIZE    = 86;
constexpr int TILE_HEIGHT  = 150;
constexpr int MINI         = 36;

// Approximates the padding the old ".lattice-tile > box" CSS rule gave
// the (now-removed) inner GtkBox. Tiles are drawn by hand now (see
// Section 11), so this is baked directly into the draw code instead
// of coming from CSS.
constexpr int TILE_PADDING = 6;

// Number of tiles per row in the flow grid. Kept as a single named
// constant because the manual keyboard-navigation logic (see
// move_selection()) needs to agree exactly with the flowbox's
// min/max-children-per-line setting in build_skeleton() -- if the two
// drift apart, Up/Down will jump to the wrong tile.
constexpr int GRID_COLUMNS = 7;


// ===========================================================================
// Section 1: Session / PID / process helpers
// ===========================================================================

std::string current_session_id()
{
    if (const char* s = std::getenv("XDG_SESSION_ID"))
        if (*s != '\0')
            return std::string("sid:") + s;

    std::string s;

    if (const char* d = std::getenv("XDG_SESSION_DESKTOP"))
        if (*d != '\0') s += d;

    if (const char* dis = std::getenv("DISPLAY"))
        if (*dis != '\0') { s += ":"; s += dis; }

    if (!s.empty()) return "env:" + s;

    std::ifstream f("/proc/sys/kernel/random/boot_id");
    if (f) {
        std::string id;
        std::getline(f, id);
        if (!id.empty()) return "boot:" + id;
    }

    return "t:" + std::to_string(::time(nullptr));
}


bool read_pid_file(const char* path, pid_t& out)
{
    std::ifstream f(path);
    if (!f) return false;

    long long v = 0;
    f >> v;
    if (!f || v <= 0) return false;

    out = static_cast<pid_t>(v);
    return true;
}

void write_pid_file(const char* path)
{
    std::ofstream f(path, std::ios::trunc);
    if (f) f << ::getpid() << '\n';
}

bool pid_is_lattice(pid_t pid)
{
    if (pid <= 0) return false;

    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream f(path);
    if (!f) return false;

    std::string comm;
    std::getline(f, comm);

    constexpr const char* want = "lattice";
    return comm.compare(0, std::strlen(want), want) == 0;
}

bool pid_is_alive(pid_t pid)
{
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

void terminate_and_wait(pid_t pid)
{
    if (pid <= 0 || !pid_is_alive(pid)) return;

    ::kill(pid, SIGTERM);

    for (int i = 0; i < 20; ++i) {
        if (!pid_is_alive(pid)) return;
        ::usleep(50 * 1000);
    }

    ::kill(pid, SIGKILL);
}

void shutdown_stale(const char* pid_path)
{
    pid_t pid = 0;
    if (!read_pid_file(pid_path, pid)) return;
    if (!pid_is_lattice(pid)) return;
    if (pid == ::getpid()) return;

    terminate_and_wait(pid);
}


// ===========================================================================
// Section 2: Terminal discovery
// ===========================================================================

// Returns the user's terminal emulator, or empty if none found.
std::string terminal_command()
{
    // 1. Explicit default, if set.
    if (DEFAULT_TERMINAL != nullptr && *DEFAULT_TERMINAL)
        return DEFAULT_TERMINAL;

    // 2. Respect $TERMINAL.
    if (const char* t = std::getenv("TERMINAL"); t && *t)
        return t;

    // 3. Probe common ones in order of preference.
    const char* candidates[] = {
        "kitty", "alacritty", "wezterm", "foot",
        "gnome-terminal", "konsole", "xfce4-terminal",
        "xterm",
    };

    for (const char* c : candidates) {
        std::string probe =
            std::string("command -v ") + c + " >/dev/null 2>&1";
        if (std::system(probe.c_str()) == 0)
            return c;
    }

    return {};
}


// ===========================================================================
// Section 3: Directory setup & daemon management
// ===========================================================================

bool ensure_fresh_lattice_dir()
{
    std::error_code ec;

    fs::create_directories(LATTICE_DIR, ec);
    if (ec) {
        std::fprintf(stderr,
                     "[lattice] cannot create %s: %s\n",
                     LATTICE_DIR, ec.message().c_str());
        return false;
    }

    fs::permissions(LATTICE_DIR,
                    fs::perms::owner_all,
                    fs::perm_options::replace,
                    ec);

    const std::string want = current_session_id();

    std::ifstream in(SESSION_FILE);
    std::string have;
    if (in) { std::getline(in, have); in.close(); }

    if (have == want) return true;

    std::fprintf(stderr,
                 "[lattice] new session (%s), resetting %s\n",
                 want.c_str(), LATTICE_DIR);

    shutdown_stale(DAEMON_PID_FILE);
    shutdown_stale(UI_PID_FILE);

    for (const auto& entry : fs::directory_iterator(LATTICE_DIR, ec)) {
        std::error_code rm;
        fs::remove_all(entry.path(), rm);
    }

    std::ofstream out(SESSION_FILE, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr,
                     "[lattice] cannot write session marker\n");
        return false;
    }
    out << want << '\n';

    return true;
}

bool daemon_is_running()
{
    pid_t pid = 0;
    if (!read_pid_file(DAEMON_PID_FILE, pid)) return false;
    if (!pid_is_lattice(pid)) return false;
    return pid_is_alive(pid);
}

void spawn_daemon_if_needed()
{
    if (daemon_is_running()) return;

    std::fprintf(stderr, "[lattice] starting lattice-daemon\n");

    int rc = std::system(
        "/usr/local/bin/lattice-daemon"
        ">/dev/null 2>&1 & disown"
    );

    if (rc != 0) {
        std::fprintf(stderr,
                     "[lattice] failed to spawn daemon (rc=%d)\n", rc);
        return;
    }

    for (int i = 0; i < 30; ++i) {
        struct stat st {};
        if (::stat(DB_PATH, &st) == 0) return;
        ::usleep(100 * 1000);
    }

    std::fprintf(stderr,
                 "[lattice] daemon spawned but DB not yet present\n");
}


// ===========================================================================
// Section 4: UI global state
// ===========================================================================

struct Ui
{
    GtkApplication* application = nullptr;

    GtkWidget* window   = nullptr;
    GtkWidget* main_box = nullptr;
    GtkWidget* search   = nullptr;
    GtkWidget* flow     = nullptr;
    GtkWidget* scroll   = nullptr;

    // Drill-down state: -1 means top-level, otherwise the index
    // into groups_ of the group currently being shown.
    int current_group = -1;

    // Cached DB so drill-down doesn't re-read the file every click.
    // Dropped back to a fresh, empty Database whenever the window is
    // hidden -- see hide_ui() -- so an idle lattice-ui holds no app
    // metadata or tile widgets in memory. (Decoded icon pixbufs are
    // NOT dropped here -- they live in the process-lifetime icon
    // cache in Section 8, so re-showing the launcher doesn't mean
    // re-decoding every icon from disk.)
    Database db;
    bool     db_loaded = false;

    int lock_fd = -1;

    // Ordered list of the flowbox children that are currently
    // *visible* (i.e. survive the active search filter, or every
    // child when there's no filter). This is the single source of
    // truth for keyboard grid navigation -- see refresh_visible_children()
    // and move_selection(). It is rebuilt any time the grid's contents
    // or visibility changes (populate_top_level/populate_group,
    // on_search_changed) so navigation never operates on stale state.
    std::vector<GtkWidget*> visible_children;
};

Ui g_ui;


// ===========================================================================
// Section 5: Forward declarations
// ===========================================================================

// --- Tile data / drawing ---
struct TileData;
GtkWidget* make_tile(const std::string& label,
                     const std::string& icon_name,
                     GCallback          on_click,
                     gpointer           user_data,
                     long               app_index);
GtkWidget* make_group_tile(const std::string& label,
                           const std::vector<std::string>& member_icons,
                           GCallback on_click,
                           gpointer  user_data);
GtkWidget* make_back_tile();
gboolean   on_tile_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);

// --- Icon helpers ---
GdkPixbuf* load_icon_pixbuf(const std::string& name, int size);
GdkPixbuf* load_icon_pixbuf_cached(const std::string& name, int size);
GdkPixbuf* load_icon_pixbuf_with_fallback(const std::string& name, int size);

// --- Population ---
void populate_top_level();
void populate_group(int group_index);
void clear_flow();
void ensure_db_loaded();

// --- Visibility ---
void hide_ui();
void show_ui();
void toggle_ui();

// --- Interaction ---
void launch_app(std::size_t app_index);
void show_tile_context_menu(GtkWidget* tile, GdkEventButton* event,
                            std::size_t app_index);
void wire_common_tile_behavior(GtkWidget* tile, long app_index);

// --- Grid navigation (keyboard) ---
void        refresh_visible_children();
int         visible_index_of(GtkWidget* child);
GtkWidget*  current_visible_child();
void        select_and_focus_child(GtkWidget* child);
gboolean    move_selection(int delta_row, int delta_col);

// --- Focus-follows-scroll (single window-level handler; see Section 13) ---
void scroll_widget_into_view(GtkWidget* widget);
void on_window_set_focus(GtkWindow*, GtkWidget* widget, gpointer);

// --- Callbacks ---
gboolean on_search_changed(GtkSearchEntry* entry, gpointer);
void     on_search_activate(GtkSearchEntry*, gpointer);
void     on_show_desktop_file_activate(GtkMenuItem*, gpointer);
void     on_edit_launcher_activate(GtkMenuItem*, gpointer);
void     on_launch_clicked(GtkButton*, gpointer);
void     on_group_clicked(GtkButton*, gpointer);
void     on_back_clicked(GtkButton*, gpointer);
gboolean on_tile_button_press(GtkWidget*, GdkEventButton*, gpointer);
gboolean on_scroll_event(GtkWidget*, GdkEventScroll*, gpointer);
gboolean on_flow_key_press(GtkWidget*, GdkEventKey*, gpointer);
gboolean on_search_key_press(GtkWidget*, GdkEventKey*, gpointer);
void     on_entry_changed(GtkEditable*, gpointer);
gboolean do_search(gpointer);
void     on_flow_child_activated(GtkFlowBox*, GtkFlowBoxChild*, gpointer);
void     on_flow_selected_changed(GtkFlowBox*, gpointer);
gboolean on_delete_event(GtkWidget*, GdkEvent*, gpointer);
gboolean on_key_press(GtkWidget*, GdkEventKey*, gpointer);
gboolean on_unix_sigusr1(gpointer);
void     activate(GtkApplication*, gpointer);
gboolean on_back_tile_button_press(GtkWidget*, GdkEventButton*, gpointer);
void     on_back_menu_activate(GtkMenuItem*, gpointer);

// ===========================================================================
// Section 6: Fuzzy matching
// ===========================================================================

static bool fuzzy_match(const std::string& needle, const std::string& hay)
{
    if (needle.empty()) return true;

    std::size_t ni = 0;
    for (std::size_t hi = 0; hi < hay.size() && ni < needle.size(); ++hi)
        if (hay[hi] == needle[ni]) ++ni;

    return ni + 1 >= needle.size();   // allow one missing char
}


// ===========================================================================
// Section 7: CSS
// ===========================================================================

constexpr const char* LATTICE_CSS = R"CSS(
.lattice-root {
    background: rgba(15, 15, 15, 0.94);
}

.lattice-tile-group {
    background: rgba(244, 67, 54, 0.10);
    border-color: rgba(244, 67, 54, 0.35);
}

.lattice-tile-group:hover {
    background: rgba(244, 67, 54, 0.18);
    border-color: rgba(244, 67, 54, 0.6);
}

.lattice-search {
    min-height: 40px;
    border-radius: 20px;
    padding: 0 16px;
    margin: 15px 0;
    font-size: 14px;
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.10);
    color: #f0f0f0;
    transition: background 160ms ease, border-color 160ms ease, box-shadow 160ms ease;
}

.lattice-search:focus,
.lattice-search:focus-within {
    background: rgba(255, 255, 255, 0.10);
    border-color: rgba(244, 67, 54, 0.55);
    box-shadow: 0 0 0 2px rgba(244, 67, 54, 0.18);
}

.lattice-search entry {
    background: transparent;
    border: none;
    box-shadow: none;
    color: #f0f0f0;
    caret-color: #f44336;
}

.lattice-tile {
    background: rgba(255, 255, 255, 0.045);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
    padding: 0;
    margin: 0;
    min-width: 0;
    min-height: 0;
    outline: none;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.25);
    transition: background 120ms ease, border-color 120ms ease,
                box-shadow 120ms ease;
}

.lattice-tile:hover {
    background: rgba(255, 255, 255, 0.10);
    border-color: rgba(244, 67, 54, 0.45);
}

.lattice-tile:active {
    background: rgba(244, 67, 54, 0.22);
    border-color: rgba(244, 67, 54, 0.85);
}

.lattice-flow {
    background: transparent;
}

.lattice-scroll scrollbar slider {
    background: rgba(255, 255, 255, 0.18);
    border-radius: 4px;
    min-width: 6px;
}

.lattice-scroll scrollbar slider:hover {
    background: rgba(255, 255, 255, 0.32);
}

.lattice-flow flowboxchild {
    border-radius: 14px;
    padding: 0;
    margin: 0;
    min-width: 0;
    min-height: 0;
    outline: none;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.25);
}
)CSS";

void install_css()
{
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, LATTICE_CSS, -1, nullptr);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);

    g_object_unref(provider);
}


// ===========================================================================
// Section 8: Icon helpers
//
// load_icon_pixbuf() is the raw, uncached decode -- exactly as before.
// Everything else in the launcher should go through
// load_icon_pixbuf_cached() (or load_icon_pixbuf_with_fallback() for
// spots that want a generic icon when lookup fails) so that repeated
// tile rebuilds -- which happen on every populate_top_level()/
// populate_group() call, i.e. every time the window is shown -- don't
// re-decode the same icon files or re-walk the icon theme over and
// over. The cache is keyed by "name@size" and lives for the process
// lifetime; entries (including failed lookups, cached as nullptr) are
// never evicted, so a pathological number of *distinct* icon
// name/size pairs could grow this cache unboundedly, but for a
// launcher's icon set that's a non-issue in practice.
//
// IMPORTANT: pixbufs returned by the cached helpers below are
// borrowed references owned by the cache. Callers must NOT
// g_object_unref() them.
// ===========================================================================

GdkPixbuf* load_icon_pixbuf(const std::string& name, int size)
{
    if (name.empty()) return nullptr;

    const bool looks_like_path =
        name[0] == '/' ||
        name.find(".png")  != std::string::npos ||
        name.find(".svg")  != std::string::npos ||
        name.find(".xpm")  != std::string::npos ||
        name.find(".jpg")  != std::string::npos ||
        name.find(".jpeg") != std::string::npos;

    if (looks_like_path) {
        GError* error = nullptr;
        GdkPixbuf* pix = gdk_pixbuf_new_from_file_at_scale(
            name.c_str(), size, size, TRUE, &error);

        if (!pix && error) {
            std::fprintf(stderr,
                         "[lattice] icon file %s: %s\n",
                         name.c_str(), error->message);
            g_error_free(error);
        }
        return pix;   // may be nullptr -> caller falls back
    }

    return gtk_icon_theme_load_icon(
        gtk_icon_theme_get_default(),
        name.c_str(),
        size,
        GTK_ICON_LOOKUP_FORCE_SIZE,
        nullptr);
}

GdkPixbuf* load_icon_pixbuf_cached(const std::string& name, int size)
{
    static std::unordered_map<std::string, GdkPixbuf*> cache;

    const std::string key = name + "@" + std::to_string(size);

    auto it = cache.find(key);
    if (it != cache.end()) return it->second;   // may be nullptr; that's cached too

    GdkPixbuf* pix = load_icon_pixbuf(name, size);
    cache.emplace(key, pix);
    return pix;
}

// Same as load_icon_pixbuf_cached(), but falls back to a generic
// "application-x-executable" icon (also cached, per size) instead of
// returning nullptr. Matches the old make_icon() behaviour for
// single-icon tiles. NOT used for group-tile mini icons, where a
// missing member icon should render as blank space, not a generic
// icon cluttering the 2x2 grid.
GdkPixbuf* load_icon_pixbuf_with_fallback(const std::string& name, int size)
{
    if (GdkPixbuf* pix = load_icon_pixbuf_cached(name, size))
        return pix;

    static std::unordered_map<int, GdkPixbuf*> fallback_cache;

    auto it = fallback_cache.find(size);
    if (it != fallback_cache.end()) return it->second;

    GdkPixbuf* fallback = gtk_icon_theme_load_icon(
        gtk_icon_theme_get_default(),
        "application-x-executable",
        size,
        GTK_ICON_LOOKUP_FORCE_SIZE,
        nullptr);

    fallback_cache.emplace(size, fallback);
    return fallback;
}

// Fixed look for hand-drawn tile labels (see Section 11). Tiles used
// to get this from the ".lattice-tile-label" CSS class on a GtkLabel;
// now that labels are painted directly with cairo/pango there's no
// GtkLabel for that CSS rule to match, so the same visual values are
// baked in here once instead.
const GdkRGBA& tile_label_color()
{
    static GdkRGBA color;
    static bool    parsed = false;

    if (!parsed) {
        if (!gdk_rgba_parse(&color, "#eaeaea"))
            color = GdkRGBA{ 0.92, 0.92, 0.92, 1.0 };
        parsed = true;
    }
    return color;
}

PangoFontDescription* tile_label_font()
{
    static PangoFontDescription* desc = nullptr;

    if (desc == nullptr) {
        desc = pango_font_description_new();
        pango_font_description_set_family(desc, "sans");
        pango_font_description_set_size(desc, 9 * PANGO_SCALE);   // ~12px
        pango_font_description_set_weight(desc, PANGO_WEIGHT_MEDIUM);
    }
    return desc;
}


// ===========================================================================
// Section 9: Launching & editing
// ===========================================================================

std::string strip_field_codes(const std::string& exec)
{
    std::string out;
    out.reserve(exec.size());

    for (std::size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            char c = exec[i + 1];

            // Standard Exec= field codes (Desktop Entry spec) -- all
            // of these get dropped since we're launching with no
            // associated file/URL context.
            if (c == 'f' || c == 'F' || c == 'u' || c == 'U' ||
                c == 'd' || c == 'D' || c == 'n' || c == 'N' ||
                c == 'i' || c == 'c' || c == 'k' || c == 'v' || c == 'm')
            {
                ++i;
                continue;
            }

            if (c == '%') { out += '%'; ++i; continue; }
        }

        out += exec[i];
    }

    return out;
}

void launch_app(std::size_t app_index)
{
    const auto& apps = g_ui.db.apps();
    if (app_index >= apps.size()) return;

    const auto& app = apps[app_index];

    std::string cmd = strip_field_codes(app.exec);
    if (cmd.empty()) {
        std::fprintf(stderr, "[lattice] no exec command for %s\n",
                     app.name.c_str());
        return;
    }

    // Parse the Exec= line into argv.
    GError* error = nullptr;
    gint    argc  = 0;
    gchar** argv  = nullptr;

    if (!g_shell_parse_argv(cmd.c_str(), &argc, &argv, &error)) {
        std::fprintf(stderr, "[lattice] bad exec line for %s: %s\n",
                     app.name.c_str(),
                     error ? error->message : "parse error");
        if (error) g_error_free(error);
        return;
    }

    // For terminal apps, prepend the terminal emulator.
    std::vector<std::string> args;
    if (app.terminal) {
        std::string term = terminal_command();
        if (term.empty()) {
            std::fprintf(stderr,
                         "[lattice] %s needs a terminal but none found\n",
                         app.name.c_str());
            g_strfreev(argv);
            return;
        }

        args.push_back(term);
        args.push_back("-e");

        for (int i = 0; i < argc; ++i)
            args.push_back(argv[i]);
    } else {
        for (int i = 0; i < argc; ++i)
            args.push_back(argv[i]);
    }

    g_strfreev(argv);

    // Build a NULL-terminated argv for g_spawn_async.
    std::vector<char*> raw;
    raw.reserve(args.size() + 1);
    for (auto& s : args) raw.push_back(const_cast<char*>(s.c_str()));
    raw.push_back(nullptr);

    GPid pid = 0;
    gboolean ok = g_spawn_async(
        nullptr, raw.data(), nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH),
        nullptr, nullptr, &pid, &error);

    if (!ok) {
        std::fprintf(stderr, "[lattice] failed to launch %s: %s\n",
                     app.name.c_str(),
                     error ? error->message : "unknown error");
        if (error) g_error_free(error);
    } else {
        g_spawn_close_pid(pid);
        std::fprintf(stderr, "[lattice] launched %s\n",
                     app.name.c_str());
        hide_ui();
    }
}

void on_show_desktop_file_activate(GtkMenuItem*, gpointer user_data)
{
    auto* path = static_cast<std::string*>(user_data);

    // Open the parent directory with the file manager.
    std::string dir = std::filesystem::path(*path).parent_path().string();
    if (dir.empty()) dir = *path;   // fallback: open the file itself

    gchar* quoted = g_shell_quote(dir.c_str());
    std::string cmd = std::string("xdg-open ") + quoted;
    g_free(quoted);

    GError* error = nullptr;
    if (!g_spawn_command_line_async(cmd.c_str(), &error)) {
        std::fprintf(stderr,
                     "[lattice] failed to xdg-open %s: %s\n",
                     dir.c_str(),
                     error ? error->message : "unknown");
        if (error) g_error_free(error);
    }
    hide_ui();
}

void on_edit_launcher_activate(GtkMenuItem*, gpointer user_data)
{
    auto* path = static_cast<std::string*>(user_data);

    gchar* quoted = g_shell_quote(path->c_str());
    std::string cmd = std::string("exo-desktop-item-edit ") + quoted;
    g_free(quoted);

    GError* error = nullptr;
    gchar** argv = nullptr;
    gint    argc = 0;

    if (!g_shell_parse_argv(cmd.c_str(), &argc, &argv, &error)) {
        std::fprintf(stderr, "[lattice] bad editor cmd: %s\n",
                     error ? error->message : "parse error");
        if (error) g_error_free(error);
        return;
    }

    GPid pid = 0;
    if (!g_spawn_async(
            nullptr, argv, nullptr,
            static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                                     G_SPAWN_DO_NOT_REAP_CHILD),
            nullptr, nullptr, &pid, &error))
    {
        std::fprintf(stderr, "[lattice] editor spawn failed: %s\n",
                     error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_strfreev(argv);
        return;
    }

    g_strfreev(argv);

    // Hide the launcher cleanly before the editor takes focus.
    hide_ui();

    // When the editor exits, poke the daemon to rescan, wait for the
    // DB to actually change, then bring the launcher back.
    g_child_watch_add(pid, [](GPid pid, gint, gpointer) {
        g_spawn_close_pid(pid);

        // Snapshot the DB's mtime *before* poking the daemon.
        struct stat before {};
        bool have_before = (::stat(DB_PATH, &before) == 0);

        // Ask the daemon to rescan (SIGUSR2 -> Daemon::rescan()).
        pid_t dpid = 0;
        if (read_pid_file(DAEMON_PID_FILE, dpid) &&
            pid_is_lattice(dpid) &&
            pid_is_alive(dpid))
        {
            ::kill(dpid, SIGUSR2);

            // Wait up to ~1s for the DB mtime to change.
            for (int i = 0; i < 20; ++i) {
                ::usleep(50 * 1000);

                struct stat after {};
                if (::stat(DB_PATH, &after) != 0)
                    continue;

                if (!have_before ||
                    after.st_mtime != before.st_mtime)
                    break;
            }
        } else {
            std::fprintf(stderr,
                         "[lattice] daemon not running; "
                         "DB may be stale after edit\n");
        }

        show_ui();   // db_loaded=false + populate_top_level()
    }, nullptr);
}

void show_tile_context_menu(GtkWidget*, GdkEventButton* event,
                            std::size_t app_index)
{
    const auto& apps = g_ui.db.apps();
    if (app_index >= apps.size()) return;

    const std::string& desktop_path = apps[app_index].desktop_file;

    if (desktop_path.empty()) {
        std::fprintf(stderr,
                     "[lattice] no .desktop path known for %s\n",
                     apps[app_index].name.c_str());
        return;
    }

    GtkWidget* menu = gtk_menu_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(menu), "lattice-context-menu");

    // --- Edit Launcher ---
    GtkWidget* edit_item = gtk_menu_item_new_with_label("    Edit Launcher");
    auto* edit_path = new std::string(desktop_path);

    g_signal_connect_data(
        edit_item, "activate",
        G_CALLBACK(on_edit_launcher_activate),
        edit_path,
        [](gpointer data, GClosure*) {
            delete static_cast<std::string*>(data);
        },
        static_cast<GConnectFlags>(0));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit_item);

    // --- Show .desktop File ---
    GtkWidget* show_item =
        gtk_menu_item_new_with_label("      Show File");
    auto* show_path = new std::string(desktop_path);

    g_signal_connect_data(
        show_item, "activate",
        G_CALLBACK(on_show_desktop_file_activate),
        show_path,
        [](gpointer data, GClosure*) {
            delete static_cast<std::string*>(data);
        },
        static_cast<GConnectFlags>(0));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(GTK_MENU(menu),
                              reinterpret_cast<GdkEvent*>(event));
}

void on_back_menu_activate(GtkMenuItem*, gpointer)
{
    populate_top_level();
}

gboolean on_back_tile_button_press(GtkWidget*, GdkEventButton* event, gpointer)
{
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkWidget* menu = gtk_menu_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(menu), "lattice-context-menu");

    // --- Go to Top ---
    GtkWidget* top_item =
        gtk_menu_item_new_with_label("Go to Top");

    g_signal_connect(top_item, "activate",
                     G_CALLBACK(on_back_menu_activate), nullptr);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top_item);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(GTK_MENU(menu),
                              reinterpret_cast<GdkEvent*>(event));
    return TRUE;
}


// ===========================================================================
// Section 10: Shared tile behavior & click handlers
// ===========================================================================

void on_launch_clicked(GtkButton*, gpointer data)
{
    launch_app(static_cast<std::size_t>(GPOINTER_TO_SIZE(data)));
}

void on_group_clicked(GtkButton*, gpointer data)
{
    const int index = GPOINTER_TO_INT(data);
    populate_group(index);
}

void on_back_clicked(GtkButton*, gpointer)
{
    populate_top_level();
}

gboolean on_tile_button_press(GtkWidget*, GdkEventButton* event, gpointer data)
{
    if (event->type == GDK_BUTTON_PRESS &&
        event->button == GDK_BUTTON_SECONDARY)
    {
        show_tile_context_menu(
            nullptr, event,
            static_cast<std::size_t>(GPOINTER_TO_SIZE(data)));
        return TRUE; // swallow it -- don't let GtkButton treat this as a click
    }
    return FALSE;
}

// Wires the behaviour every *app* tile needs on top of the shared
// click handler: a right-click context menu for editing/showing its
// .desktop file. Non-app tiles (groups, back) pass app_index < 0 and
// get no extra wiring -- keyboard-focus scrolling used to be wired
// per-tile here too, but that's now handled by a single window-level
// "set-focus" handler (see Section 13 / on_window_set_focus), so
// there is nothing to connect for them at all.
void wire_common_tile_behavior(GtkWidget* tile, long app_index)
{
    if (app_index >= 0) {
        gtk_widget_add_events(tile, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(tile, "button-press-event",
                         G_CALLBACK(on_tile_button_press),
                         GSIZE_TO_POINTER(static_cast<std::size_t>(app_index)));
    }
}


// ===========================================================================
// Section 11: Tile builders
//
// Tiles used to be a GtkButton -> GtkBox -> (GtkImage, GtkLabel) tree
// (4 widgets each). With hundreds of apps that's a lot of widget
// overhead for what's fundamentally "one icon + one line of text".
// Tiles are now a GtkButton -> GtkDrawingArea (2 widgets), with the
// icon(s) and label painted directly in a "draw" handler
// (on_tile_draw) driven by a small TileData struct attached to the
// button. This keeps every behavioural hook that depends on there
// being exactly one child widget under the button (context menus,
// search-matching, keynav) working unchanged -- they just read
// TileData instead of walking GtkLabel/GtkBox children.
// ===========================================================================

// Everything on_tile_draw() needs to paint a tile. `icons` holds one
// borrowed (cache-owned, never unref'd here) pixbuf for a normal/back
// tile, or up to four (with possible nullptr gaps) for a group tile's
// 2x2 mini-grid.
struct TileData
{
    std::string             label;
    std::vector<GdkPixbuf*> icons;
    bool                    is_group = false;
    bool                    is_back  = false;
};

void tile_data_free(gpointer data)
{
    delete static_cast<TileData*>(data);
}

gboolean on_tile_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
    auto* td = static_cast<TileData*>(user_data);
    if (td == nullptr) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    const int width  = alloc.width;
    const int height = alloc.height;

    int icon_bottom = TILE_PADDING;

    if (td->is_group) {
        // 2x2 mini icon grid, centered horizontally.
        constexpr int spacing = 3;
        const int grid_w = MINI * 2 + spacing;
        const int gx0    = (width - grid_w) / 2;
        const int gy0    = TILE_PADDING + 8;

        for (int i = 0; i < 4; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            const int x   = gx0 + col * (MINI + spacing);
            const int y   = gy0 + row * (MINI + spacing);

            if (i < static_cast<int>(td->icons.size()) && td->icons[i] != nullptr) {
                gdk_cairo_set_source_pixbuf(cr, td->icons[i], x, y);
                cairo_paint(cr);
            }
        }

        icon_bottom = gy0 + MINI * 2 + spacing;
    } else if (!td->icons.empty() && td->icons[0] != nullptr) {
        GdkPixbuf* pix = td->icons[0];
        const int  iw  = gdk_pixbuf_get_width(pix);
        const int  ih  = gdk_pixbuf_get_height(pix);
        const int  x   = (width - iw) / 2;
        const int  y   = TILE_PADDING + 6;

        gdk_cairo_set_source_pixbuf(cr, pix, x, y);
        cairo_paint(cr);

        icon_bottom = y + ih;
    }

    if (td->label.empty()) return FALSE;

    PangoLayout* layout = gtk_widget_create_pango_layout(widget, td->label.c_str());
    pango_layout_set_font_description(layout, tile_label_font());
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, std::max(1, width - 2 * TILE_PADDING) * PANGO_SCALE);
    pango_layout_set_height(layout, -2);   // clamp to at most 2 lines

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    double lx = TILE_PADDING;
    double ly = height - text_h - TILE_PADDING;
    if (ly < icon_bottom + 2) ly = icon_bottom + 2;

    const GdkRGBA& color = tile_label_color();

    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_move_to(cr, lx, ly);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
    return FALSE;
}

// Shared skeleton for all three tile kinds: a borderless GtkButton
// containing one GtkDrawingArea, with TileData wired up for
// on_tile_draw(). Returns the canvas so callers can finish wiring
// their own "draw" data (each tile kind has different icon/label
// content, filled in by the caller before this returns control).
GtkWidget* make_tile_shell(GtkWidget** out_canvas)
{
    GtkWidget* tile = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(tile), GTK_RELIEF_NONE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(tile), "lattice-tile");

    gtk_widget_set_can_focus(tile, TRUE);
    gtk_widget_set_size_request(tile, -1, TILE_HEIGHT);
    gtk_widget_set_hexpand(tile, TRUE);
    gtk_widget_set_vexpand(tile, FALSE);
    gtk_widget_set_halign(tile, GTK_ALIGN_FILL);
    gtk_widget_set_valign(tile, GTK_ALIGN_START);

    GtkWidget* canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(canvas, TRUE);
    gtk_widget_set_vexpand(canvas, TRUE);
    gtk_container_add(GTK_CONTAINER(tile), canvas);

    *out_canvas = canvas;
    return tile;
}

// app_index: pass the real index into db.apps() for a launchable tile
// (enables the right-click "Edit Launcher" menu); pass -1 for
// non-app tiles (groups, back).
GtkWidget* make_tile(
    const std::string& label,
    const std::string& icon_name,
    GCallback          on_click,
    gpointer           user_data,
    long               app_index)
{
    GtkWidget* canvas = nullptr;
    GtkWidget* tile   = make_tile_shell(&canvas);

    auto* td = new TileData();
    td->label = label;
    td->icons.push_back(load_icon_pixbuf_with_fallback(icon_name, ICON_SIZE));

    g_object_set_data_full(G_OBJECT(tile), "lattice-tile-data",
                           td, tile_data_free);
    g_signal_connect(canvas, "draw", G_CALLBACK(on_tile_draw), td);

    if (on_click != nullptr)
        g_signal_connect(tile, "clicked", on_click, user_data);

    wire_common_tile_behavior(tile, app_index);

    return tile;
}

// A group tile: same fixed size, but a 2x2 mini-grid icon drawn
// instead of a single big one.
GtkWidget* make_group_tile(
    const std::string& label,
    const std::vector<std::string>& member_icons,
    GCallback on_click,
    gpointer  user_data)
{
    GtkWidget* canvas = nullptr;
    GtkWidget* tile   = make_tile_shell(&canvas);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(tile), "lattice-tile-group");
    gtk_widget_set_hexpand(tile, FALSE);

    auto* td = new TileData();
    td->label    = label;
    td->is_group = true;

    const int count = static_cast<int>(member_icons.size());
    for (int i = 0; i < 4; ++i) {
        if (i < count && !member_icons[i].empty())
            // No fallback here on purpose: a missing member icon
            // should render as blank space in the 2x2 grid, not a
            // generic icon, matching the pre-refactor behaviour.
            td->icons.push_back(load_icon_pixbuf_cached(member_icons[i], MINI));
        else
            td->icons.push_back(nullptr);
    }

    g_object_set_data_full(G_OBJECT(tile), "lattice-tile-data",
                           td, tile_data_free);
    g_signal_connect(canvas, "draw", G_CALLBACK(on_tile_draw), td);

    g_signal_connect(tile, "clicked", on_click, user_data);

    wire_common_tile_behavior(tile, -1);

    return tile;
}

GtkWidget* make_back_tile()
{
    GtkWidget* canvas = nullptr;
    GtkWidget* tile   = make_tile_shell(&canvas);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(tile), "lattice-tile-back");
    gtk_widget_set_hexpand(tile, FALSE);

    auto* td = new TileData();
    td->label   = "Back";
    td->is_back = true;
    td->icons.push_back(load_icon_pixbuf_with_fallback("go-previous", ICON_SIZE));

    g_object_set_data_full(G_OBJECT(tile), "lattice-tile-data",
                           td, tile_data_free);
    g_signal_connect(canvas, "draw", G_CALLBACK(on_tile_draw), td);

    g_signal_connect(tile, "clicked",
                     G_CALLBACK(on_back_clicked), nullptr);

    gtk_widget_add_events(tile, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(tile, "button-press-event",
                     G_CALLBACK(on_back_tile_button_press), nullptr);

    wire_common_tile_behavior(tile, -1);

    return tile;
}


// ===========================================================================
// Section 12: Flow management & DB cache
// ===========================================================================

// Case-insensitive alphabetical comparator for tile labels.
static bool tile_label_less(const std::string& a, const std::string& b)
{
    std::string la = a, lb = b;
    std::transform(la.begin(), la.end(), la.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(lb.begin(), lb.end(), lb.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return la < lb;
}

void clear_flow()
{
    if (g_ui.flow == nullptr) return;

    GList* children =
        gtk_container_get_children(GTK_CONTAINER(g_ui.flow));

    for (GList* l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));

    g_list_free(children);

    // The grid is gone -- any cached navigation state pointing at its
    // children would be dangling. Drop it immediately rather than
    // waiting for the next refresh_visible_children() call.
    g_ui.visible_children.clear();
}

void ensure_db_loaded()
{
    if (g_ui.db_loaded) return;

    if (g_ui.db.load(DB_PATH)) {
        g_ui.db_loaded = true;
        std::fprintf(stderr,
                     "[lattice] loaded %zu applications, %zu groups\n",
                     g_ui.db.apps().size(),
                     g_ui.db.groups().size());
    } else {
        std::fprintf(stderr,
                     "[lattice] failed to load %s\n", DB_PATH);
    }
}


// ===========================================================================
// Section 13: Scrolling, keyboard grid navigation & focus-follows-scroll
// ===========================================================================
//
// Arrow-key navigation used to be delegated entirely to GtkFlowBox's
// built-in keynav. That works fine for a static grid, but breaks down
// as soon as the search filter starts hiding tiles: GtkFlowBox's idea
// of "next child" doesn't reliably skip invisible flowbox children,
// and a selection left over from before a filter was applied can
// point at a tile that's no longer visible. The symptoms are the
// classic "arrow keys do nothing" / "arrow keys jump to a hidden
// tile" bugs.
//
// To make this deterministic, keyboard navigation here is driven
// entirely off `g_ui.visible_children` -- an explicitly maintained,
// ordered list of the flowbox children that currently pass the
// search filter (all of them, when there's no filter). Anything that
// changes which tiles are visible (populate_top_level, populate_group,
// on_search_changed) must call refresh_visible_children() afterwards,
// and should clear any stale flowbox selection so it can't refer to
// a tile that just got hidden.

void scroll_by(GtkWidget* scroll, double dy)
{
    if (scroll == nullptr) return;

    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(scroll));

    if (adj == nullptr) return;

    double value = gtk_adjustment_get_value(adj) + dy;
    double upper = gtk_adjustment_get_upper(adj);
    double page  = gtk_adjustment_get_page_size(adj);

    value = std::clamp(value, 0.0, std::max(0.0, upper - page));
    gtk_adjustment_set_value(adj, value);
}

gboolean on_scroll_event(GtkWidget*, GdkEventScroll* event, gpointer)
{
    double step = TILE_HEIGHT + 6;

    switch (event->direction) {
        case GDK_SCROLL_UP:   scroll_by(g_ui.scroll, -step); return TRUE;
        case GDK_SCROLL_DOWN: scroll_by(g_ui.scroll,  step); return TRUE;
        default: return FALSE;
    }
}

// Scrolls `widget` into view within g_ui.scroll, if it's a descendant
// of g_ui.flow. Used to keep the keyboard-focused tile visible --
// previously wired as a "focus-in-event" handler on every single
// tile; now called once from a single window-level "set-focus"
// handler (on_window_set_focus, below), which fires for whatever
// widget GTK just focused, tile or not. Non-tile targets (e.g. the
// search entry) simply fail the "is this under g_ui.flow" walk and
// return early.
void scroll_widget_into_view(GtkWidget* widget)
{
    if (widget == nullptr || g_ui.scroll == nullptr || g_ui.flow == nullptr)
        return;

    // Walk up from the focused widget to find the direct flowbox
    // child that contains it. If we hit the flowbox itself or run
    // out of parents first, this focus event isn't about a tile.
    GtkWidget* w = widget;
    while (w != nullptr && w != g_ui.flow && gtk_widget_get_parent(w) != g_ui.flow)
        w = gtk_widget_get_parent(w);

    if (w == nullptr || w == g_ui.flow) return;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    gint tile_y = 0;
    gtk_widget_translate_coordinates(
        widget, g_ui.flow, 0, 0, nullptr, &tile_y);

    GtkAdjustment* adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_ui.scroll));
    if (adj == nullptr) return;

    double top         = tile_y;
    double bottom      = tile_y + alloc.height;
    double view_top    = gtk_adjustment_get_value(adj);
    double view_bottom = view_top + gtk_adjustment_get_page_size(adj);

    if (top < view_top)
        gtk_adjustment_set_value(adj, top);
    else if (bottom > view_bottom)
        gtk_adjustment_set_value(
            adj, bottom - gtk_adjustment_get_page_size(adj));
}

void on_window_set_focus(GtkWindow*, GtkWidget* widget, gpointer)
{
    scroll_widget_into_view(widget);
}

// Rebuilds the ordered list of visible flowbox children. Call this
// any time tiles are added/removed/re-shown/re-hidden. Cheap enough
// (a handful of pointer comparisons) to call liberally rather than
// trying to track deltas.
void refresh_visible_children()
{
    g_ui.visible_children.clear();

    if (g_ui.flow == nullptr) return;

    GList* children = gtk_container_get_children(GTK_CONTAINER(g_ui.flow));

    for (GList* l = children; l; l = l->next) {
        GtkWidget* w = GTK_WIDGET(l->data);

        if (GTK_IS_FLOW_BOX_CHILD(w) && gtk_widget_get_visible(w))
            g_ui.visible_children.push_back(w);
    }

    g_list_free(children);
}

int visible_index_of(GtkWidget* child)
{
    if (child == nullptr) return -1;

    for (std::size_t i = 0; i < g_ui.visible_children.size(); ++i)
        if (g_ui.visible_children[i] == child)
            return static_cast<int>(i);

    return -1;
}

// Finds the flowbox child that's currently "current" for navigation
// purposes: prefer the flowbox's own selection (kept in sync with
// keyboard focus by on_flow_selected_changed), but fall back to
// walking up from whatever widget actually holds keyboard focus, in
// case the two have drifted apart.
GtkWidget* current_visible_child()
{
    if (g_ui.flow == nullptr) return nullptr;

    GList* sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(g_ui.flow));
    GtkWidget* child = nullptr;

    if (sel != nullptr) {
        child = GTK_WIDGET(sel->data);
        g_list_free(sel);
    }

    if (child != nullptr && gtk_widget_get_visible(child))
        return child;

    if (g_ui.window == nullptr) return nullptr;

    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(g_ui.window));
    while (focus != nullptr && !GTK_IS_FLOW_BOX_CHILD(focus))
        focus = gtk_widget_get_parent(focus);

    if (focus != nullptr && gtk_widget_get_visible(focus))
        return focus;

    return nullptr;
}

// Selects `child` in the flowbox and moves real keyboard focus onto
// its tile. Selection and focus are kept in lockstep deliberately --
// see on_flow_selected_changed -- so this one call is enough to make
// both the flowbox's internal state and the widget-focus chain agree.
void select_and_focus_child(GtkWidget* child)
{
    if (child == nullptr || g_ui.flow == nullptr || !GTK_IS_FLOW_BOX_CHILD(child))
        return;

    gtk_flow_box_select_child(GTK_FLOW_BOX(g_ui.flow),
                              GTK_FLOW_BOX_CHILD(child));

    GtkWidget* tile = gtk_bin_get_child(GTK_BIN(child));
    if (tile != nullptr)
        gtk_widget_grab_focus(tile);
}

// Manual replacement for GtkFlowBox's built-in arrow-key navigation.
// delta_row/delta_col are in grid units: e.g. (-1, 0) for Up,
// (0, +1) for Right. Left/Right step one tile at a time (wrapping
// across row boundaries); Up/Down jump a full row (GRID_COLUMNS
// tiles), which is what "moving up/down the grid" means visually.
//
// Always operates against the current g_ui.visible_children list, so
// a tile hidden by the search filter is never a valid destination.
gboolean move_selection(int delta_row, int delta_col)
{
    if (g_ui.visible_children.empty()) return FALSE;

    const int count = static_cast<int>(g_ui.visible_children.size());

    GtkWidget* current = current_visible_child();
    int idx = visible_index_of(current);

    if (idx < 0) {
        // Nothing usable is currently selected/focused (e.g. right
        // after a filter changed) -- just land on the first tile.
        select_and_focus_child(g_ui.visible_children.front());
        return TRUE;
    }

    int new_idx = idx;

    if (delta_col != 0) {
        new_idx = std::clamp(idx + delta_col, 0, count - 1);
    } else if (delta_row != 0) {
        new_idx = idx + delta_row * GRID_COLUMNS;
        new_idx = std::clamp(new_idx, 0, count - 1);
    }

    select_and_focus_child(g_ui.visible_children[new_idx]);
    return TRUE;
}

gboolean on_flow_key_press(GtkWidget*, GdkEventKey* event, gpointer)
{
    double step = TILE_HEIGHT + 6;

    switch (event->keyval) {
        case GDK_KEY_Up:
            return move_selection(-1, 0);

        case GDK_KEY_Down:
            return move_selection(1, 0);

        case GDK_KEY_Left:
            return move_selection(0, -1);

        case GDK_KEY_Right:
            return move_selection(0, 1);

        case GDK_KEY_Page_Up:
            scroll_by(g_ui.scroll, -step * 3);
            return TRUE;

        case GDK_KEY_Page_Down:
            scroll_by(g_ui.scroll, step * 3);
            return TRUE;

        case GDK_KEY_Home: {
            GtkAdjustment* adj =
                gtk_scrolled_window_get_vadjustment(
                    GTK_SCROLLED_WINDOW(g_ui.scroll));
            gtk_adjustment_set_value(adj, 0.0);
            return TRUE;
        }

        case GDK_KEY_End: {
            GtkAdjustment* adj =
                gtk_scrolled_window_get_vadjustment(
                    GTK_SCROLLED_WINDOW(g_ui.scroll));
            gtk_adjustment_set_value(
                adj,
                std::max(0.0,
                    gtk_adjustment_get_upper(adj) -
                    gtk_adjustment_get_page_size(adj)));
            return TRUE;
        }

        case GDK_KEY_Escape: {
            if (g_ui.current_group >= 0)
                populate_top_level();
            else
                hide_ui();
            return TRUE;
        }
        default: {
            if (g_ui.search == nullptr) return FALSE;

            gunichar ch = gdk_keyval_to_unicode(event->keyval);
            if (ch == 0 || !g_unichar_isgraph(ch)) return FALSE;

            gchar utf8[7] = {0};
            gint  len     = g_unichar_to_utf8(ch, utf8);
            utf8[len]     = '\0';

            GtkEditable* ed = GTK_EDITABLE(g_ui.search);

            gtk_widget_grab_focus(g_ui.search);

            gint pos = gtk_editable_get_position(ed);
            gtk_editable_insert_text(ed, utf8, len, &pos);
            gtk_editable_set_position(ed, pos);

            return TRUE;
        }
        
    }
}


// ===========================================================================
// Section 14: Population -- top level & drill-down
// ===========================================================================

void populate_top_level()
{
    g_ui.current_group = -1;
    clear_flow();

    ensure_db_loaded();

    if (!g_ui.db_loaded) {
        GtkWidget* placeholder =
            gtk_label_new("No application database");
        gtk_widget_set_margin_top(placeholder, 24);
        gtk_widget_set_margin_start(placeholder, 24);
        gtk_label_set_xalign(GTK_LABEL(placeholder), 0.0f);

        gtk_flow_box_insert(GTK_FLOW_BOX(g_ui.flow),
                            placeholder, -1);
        gtk_widget_show_all(g_ui.flow);
        refresh_visible_children();
        return;
    }

    const auto& apps   = g_ui.db.apps();
    const auto& groups = g_ui.db.groups();

    // ONE list for both groups and apps.
    std::vector<std::pair<std::string, GtkWidget*>> entries;

    // --- Groups (multi-member only) ---
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& group = groups[gi];
        if (group.app_indices.size() <= 1) continue;

        std::vector<std::string> member_icons;
        for (std::size_t k = 0;
             k < group.app_indices.size() && k < 4;
             ++k)
        {
            std::size_t idx = group.app_indices[k];
            if (idx < apps.size())
                member_icons.push_back(apps[idx].icon);
        }

        GtkWidget* tile = make_group_tile(
            group.name,
            member_icons,
            G_CALLBACK(on_group_clicked),
            GINT_TO_POINTER(static_cast<int>(gi)));

        entries.emplace_back(group.name, tile);
    }

    // --- Singletons (apps not in any multi-member group) ---
    std::vector<bool> is_grouped(apps.size(), false);

    for (const auto& group : groups) {
        if (group.app_indices.size() <= 1) continue;

        for (std::size_t index : group.app_indices) {
            if (index < is_grouped.size())
                is_grouped[index] = true;
        }
    }

    for (std::size_t i = 0; i < apps.size(); ++i) {
        if (is_grouped[i]) continue;

        GtkWidget* tile = make_tile(
            apps[i].name,
            apps[i].icon,
            G_CALLBACK(on_launch_clicked),
            GSIZE_TO_POINTER(i),
            static_cast<long>(i));

        entries.emplace_back(apps[i].name, tile);
    }

    // --- ONE sort over both groups and apps ---
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return tile_label_less(a.first, b.first);
              });

    // --- Insert everything in the merged, sorted order ---
    for (auto& [label, tile] : entries)
        gtk_flow_box_insert(GTK_FLOW_BOX(g_ui.flow), tile, -1);

    gtk_widget_show_all(g_ui.flow);

    if (g_ui.search != nullptr)
        gtk_entry_set_text(GTK_ENTRY(g_ui.search), "");

    // Fresh grid, nothing filtered out -- rebuild the nav list before
    // anyone tries to press an arrow key.
    refresh_visible_children();
    gtk_widget_grab_focus(g_ui.search);

}

void populate_group(int group_index)
{
    clear_flow();

    ensure_db_loaded();

    if (!g_ui.db_loaded) {
        populate_top_level();
        return;
    }

    const auto& groups = g_ui.db.groups();

    if (group_index < 0 ||
        static_cast<std::size_t>(group_index) >= groups.size())
    {
        populate_top_level();
        return;
    }

    g_ui.current_group = group_index;

    const auto& group = groups[group_index];
    const auto& apps  = g_ui.db.apps();

    // Back tile first.
    gtk_flow_box_insert(
        GTK_FLOW_BOX(g_ui.flow), make_back_tile(), -1);

    // The group's members.
    for (std::size_t index : group.app_indices) {
        if (index >= apps.size()) continue;

        GtkWidget* tile = make_tile(
            apps[index].name,
            apps[index].icon,
            G_CALLBACK(on_launch_clicked),
            GSIZE_TO_POINTER(index),
            static_cast<long>(index));

        gtk_flow_box_insert(GTK_FLOW_BOX(g_ui.flow), tile, -1);
    }

    gtk_widget_show_all(g_ui.flow);

    if (g_ui.search != nullptr)
        gtk_entry_set_text(GTK_ENTRY(g_ui.search), "");

    // Fresh grid, nothing filtered out -- rebuild the nav list before
    // anyone tries to press an arrow key.
    refresh_visible_children();

    gtk_widget_grab_focus(g_ui.search);
}


// ===========================================================================
// Section 15: Search
// ===========================================================================

static guint search_timeout_id = 0;

gboolean do_search(gpointer)
{
    search_timeout_id = 0;
    on_search_changed(GTK_SEARCH_ENTRY(g_ui.search), nullptr);
    return G_SOURCE_REMOVE;
}

void on_entry_changed(GtkEditable*, gpointer)
{
    if (search_timeout_id != 0)
        g_source_remove(search_timeout_id);

    search_timeout_id = g_timeout_add(400, do_search, nullptr);
}

gboolean on_search_changed(GtkSearchEntry* entry, gpointer)
{
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(entry));

    std::string needle;
    if (text != nullptr) needle = text;

    std::transform(
        needle.begin(), needle.end(), needle.begin(),
        [](unsigned char c) { return std::tolower(c); });


    gtk_flow_box_unselect_all(GTK_FLOW_BOX(g_ui.flow));

    GList* children =
        gtk_container_get_children(GTK_CONTAINER(g_ui.flow));

    for (GList* l = children; l; l = l->next) {
        GtkWidget* wrapper = GTK_WIDGET(l->data);

        if (!GTK_IS_FLOW_BOX_CHILD(wrapper)) {
            gtk_widget_set_visible(wrapper, TRUE);
            continue;
        }

        GtkWidget* tile = gtk_bin_get_child(GTK_BIN(wrapper));

        if (tile == nullptr) {
            gtk_widget_set_visible(wrapper, TRUE);
            continue;
        }

        // Tiles carry their own searchable label + back-ness via
        // TileData now, rather than us walking down into GtkLabel
        // children that no longer exist. The top-level "no database"
        // placeholder is a bare GtkLabel with no TileData attached,
        // so it falls through to "always visible", same as before.
        auto* td = static_cast<TileData*>(
            g_object_get_data(G_OBJECT(tile), "lattice-tile-data"));

        bool match = needle.empty();

        if (td == nullptr) {
            match = true;
        } else if (td->is_back) {
            match = true;
        } else if (!match) {
            std::string hay = td->label;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            match = fuzzy_match(needle, hay);
        }

        gtk_widget_set_visible(wrapper, match);
    }

    g_list_free(children);

    // The set of visible tiles just changed -- rebuild the ordered
    // navigation list so Up/Down/Left/Right (and the search box's own
    // Down-arrow handoff) operate on the *new* grid, not the one from
    // before the keystroke.
    refresh_visible_children();

    return FALSE;
}

gboolean on_search_key_press(GtkWidget*, GdkEventKey* event, gpointer)
{
    if (event->keyval == GDK_KEY_Escape) {
        return TRUE;   // always consumed -- never bubbles to window
    }

    if (event->keyval == GDK_KEY_Down) {
        refresh_visible_children();

        if (!g_ui.visible_children.empty())
            select_and_focus_child(g_ui.visible_children.front());

        return TRUE;
    }

    return FALSE;
}

void on_search_activate(GtkSearchEntry*, gpointer)
{
    // Launch the first visible, non-Back tile.
    GList* children =
        gtk_container_get_children(GTK_CONTAINER(g_ui.flow));

    for (GList* l = children; l; l = l->next) {
        GtkWidget* wrapper = GTK_WIDGET(l->data);
        if (!GTK_IS_FLOW_BOX_CHILD(wrapper)) continue;
        if (!gtk_widget_get_visible(wrapper)) continue;

        GtkWidget* tile = gtk_bin_get_child(GTK_BIN(wrapper));
        if (tile == nullptr) continue;

        auto* td = static_cast<TileData*>(
            g_object_get_data(G_OBJECT(tile), "lattice-tile-data"));

        // Skip the Back tile -- it has no app to launch.
        if (td != nullptr && td->is_back) continue;

        std::fprintf(stderr, "[lattice] activate fired\n");

        if (GTK_IS_BUTTON(tile)) {
            gtk_button_clicked(GTK_BUTTON(tile));
            break;
        }
    }

    g_list_free(children);
}


// ===========================================================================
// Section 16: Flowbox signal handlers
// ===========================================================================

void on_flow_child_activated(GtkFlowBox*, GtkFlowBoxChild* child, gpointer)
{
    if (child == nullptr) return;

    GtkWidget* tile = gtk_bin_get_child(GTK_BIN(child));
    if (tile == nullptr) return;

    // Let the tile's own "clicked" handler do the work by
    // synthesizing a click.
    if (GTK_IS_BUTTON(tile))
        gtk_button_clicked(GTK_BUTTON(tile));
}

void on_flow_selected_changed(GtkFlowBox* box, gpointer)
{
    GList* sel = gtk_flow_box_get_selected_children(box);
    if (sel == nullptr) return;

    GtkFlowBoxChild* child = GTK_FLOW_BOX_CHILD(sel->data);
    if (child) {
        GtkWidget* tile = gtk_bin_get_child(GTK_BIN(child));
        if (tile) gtk_widget_grab_focus(tile);
    }
    g_list_free(sel);
}


// ===========================================================================
// Section 17: Show / hide / toggle
// ===========================================================================

void hide_ui()
{
    if (g_ui.window == nullptr) return;

    gtk_widget_hide(g_ui.window);
    clear_flow();
    g_ui.db = Database();
    g_ui.db_loaded = false;
    g_ui.current_group = -1;
}

void show_ui()
{
    if (g_ui.window == nullptr) return;

    // Re-read the DB each time the window is shown, so a daemon
    // update is reflected. Invalidate the cache.
    g_ui.db_loaded = false;
    populate_top_level();

    gtk_widget_show_all(g_ui.window);
    gtk_window_present(GTK_WINDOW(g_ui.window));

    // populate_top_level() already focuses the first tile; do it
    // again here in case show_all() reset focus.
    if (g_ui.flow != nullptr)
        gtk_widget_grab_focus(g_ui.search);
}

void toggle_ui()
{
    if (g_ui.window == nullptr) return;

    if (gtk_widget_get_visible(g_ui.window))
        hide_ui();
    else
        show_ui();
}


// ===========================================================================
// Section 18: Window signals
// ===========================================================================

gboolean on_delete_event(GtkWidget*, GdkEvent*, gpointer)
{
    hide_ui();
    return TRUE;
}

gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer)
{
    if (event->keyval == GDK_KEY_Escape) {
        if (g_ui.search != nullptr &&
            gtk_window_get_focus(GTK_WINDOW(g_ui.window)) == g_ui.search)
        {
            const gchar* text =
                gtk_entry_get_text(GTK_ENTRY(g_ui.search));

            if (text != nullptr && *text != '\0') {
                gtk_entry_set_text(GTK_ENTRY(g_ui.search), "");
                gtk_editable_set_position(
                    GTK_EDITABLE(g_ui.search), -1);

                if (search_timeout_id != 0) {
                    g_source_remove(search_timeout_id);
                    search_timeout_id = 0;
                }
                on_search_changed(
                    GTK_SEARCH_ENTRY(g_ui.search), nullptr);
            } else {
                if (g_ui.current_group >= 0)
                    populate_top_level();
                else
                    hide_ui();
                }
        }

        // Escape: if inside a group, go back. Otherwise hide.
        else if (g_ui.current_group >= 0)
            populate_top_level();
        else
            hide_ui();
        return TRUE;
    }
    return FALSE;
}


// ===========================================================================
// Section 19: SIGUSR1 handler
// ===========================================================================

gboolean on_unix_sigusr1(gpointer)
{
    toggle_ui();
    return G_SOURCE_CONTINUE;
}


// ===========================================================================
// Section 20: Skeleton construction
// ===========================================================================

void build_skeleton()
{
    install_css();

    g_ui.window = gtk_application_window_new(g_ui.application);

    gtk_window_set_title(GTK_WINDOW(g_ui.window), "Lattice");
    gtk_window_set_default_size(GTK_WINDOW(g_ui.window), 760, 640);

    // ---- Layer shell ---------------------------------------------------
    gtk_layer_init_for_window(GTK_WINDOW(g_ui.window));

    gtk_layer_set_layer(
        GTK_WINDOW(g_ui.window),
        GTK_LAYER_SHELL_LAYER_TOP);

    gtk_layer_set_anchor(
        GTK_WINDOW(g_ui.window), GTK_LAYER_SHELL_EDGE_TOP,    TRUE);
    gtk_layer_set_anchor(
        GTK_WINDOW(g_ui.window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(
        GTK_WINDOW(g_ui.window), GTK_LAYER_SHELL_EDGE_LEFT,   TRUE);
    gtk_layer_set_anchor(
        GTK_WINDOW(g_ui.window), GTK_LAYER_SHELL_EDGE_RIGHT,  TRUE);

    gtk_layer_set_exclusive_zone(GTK_WINDOW(g_ui.window), -1);
    gtk_layer_set_namespace(GTK_WINDOW(g_ui.window), "lattice-shell");

    gtk_layer_set_keyboard_mode(
        GTK_WINDOW(g_ui.window),
        GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    gtk_window_set_decorated(GTK_WINDOW(g_ui.window), FALSE);
    // --------------------------------------------------------------------

    g_signal_connect(g_ui.window, "delete-event",
                     G_CALLBACK(on_delete_event), nullptr);
    g_signal_connect(g_ui.window, "key-press-event",
                     G_CALLBACK(on_key_press), nullptr);

    // Single window-level focus tracker: replaces the old per-tile
    // "focus-in-event" handlers (one connection instead of one per
    // tile) and keeps the focused tile scrolled into view no matter
    // how focus got there (mouse click, Tab, or our own manual
    // keynav in select_and_focus_child()).
    g_signal_connect(g_ui.window, "set-focus",
                     G_CALLBACK(on_window_set_focus), nullptr);

    g_ui.main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(g_ui.main_box), "lattice-root");
    gtk_container_add(GTK_CONTAINER(g_ui.window), g_ui.main_box);

    // ---- Compact, centered search bar ----------------------------------
    GtkWidget* search_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(search_wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(search_wrap, 10);
    gtk_widget_set_margin_bottom(search_wrap, 4);
    gtk_widget_set_margin_start(search_wrap, 12);
    gtk_widget_set_margin_end(search_wrap, 12);

    g_ui.search = gtk_search_entry_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(g_ui.search), "lattice-search");
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(g_ui.search), "Search");
    gtk_widget_set_size_request(g_ui.search, 320, -1);
    gtk_widget_set_hexpand(g_ui.search, FALSE);

    g_signal_connect(g_ui.search, "search-changed",
                     G_CALLBACK(on_entry_changed), nullptr);
    g_signal_connect(g_ui.search, "key-press-event",
                     G_CALLBACK(on_search_key_press), nullptr);
    g_signal_connect(g_ui.search, "activate",
                     G_CALLBACK(on_search_activate), nullptr);

    gtk_box_pack_start(
        GTK_BOX(search_wrap), g_ui.search, FALSE, FALSE, 0);

    gtk_box_pack_start(
        GTK_BOX(g_ui.main_box), search_wrap, FALSE, FALSE, 0);

    // ---- Scrolled icon grid --------------------------------------------
    g_ui.scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(g_ui.scroll), "lattice-scroll");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(g_ui.scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(g_ui.scroll), TRUE);
    gtk_widget_add_events(g_ui.scroll, GDK_SCROLL_MASK);
    g_signal_connect(g_ui.scroll, "scroll-event",
                     G_CALLBACK(on_scroll_event), nullptr);

    gtk_box_pack_start(
        GTK_BOX(g_ui.main_box), g_ui.scroll, TRUE, TRUE, 0);

    // ---- Flow box (create FIRST, connect AFTER) ------------------------
    g_ui.flow = gtk_flow_box_new();

    gtk_widget_set_vexpand(g_ui.flow, FALSE);
    gtk_widget_set_valign(g_ui.flow, GTK_ALIGN_START);

    gtk_style_context_add_class(
        gtk_widget_get_style_context(g_ui.flow), "lattice-flow");

    gtk_flow_box_set_selection_mode(
        GTK_FLOW_BOX(g_ui.flow), GTK_SELECTION_SINGLE);

    gtk_flow_box_set_homogeneous(
        GTK_FLOW_BOX(g_ui.flow), TRUE);
    gtk_flow_box_set_row_spacing(
        GTK_FLOW_BOX(g_ui.flow), 14);
    gtk_flow_box_set_column_spacing(
        GTK_FLOW_BOX(g_ui.flow), 14);
    gtk_flow_box_set_min_children_per_line(
        GTK_FLOW_BOX(g_ui.flow), GRID_COLUMNS);
    gtk_flow_box_set_max_children_per_line(
        GTK_FLOW_BOX(g_ui.flow), GRID_COLUMNS);
    gtk_flow_box_set_activate_on_single_click(
        GTK_FLOW_BOX(g_ui.flow), TRUE);

    gtk_widget_set_margin_start(g_ui.flow, 16);
    gtk_widget_set_margin_end(g_ui.flow, 16);
    gtk_widget_set_margin_top(g_ui.flow, 8);
    gtk_widget_set_margin_bottom(g_ui.flow, 16);

    gtk_widget_set_can_focus(g_ui.flow, TRUE);

    // --- All flowbox signals go HERE, after creation ---
    g_signal_connect(g_ui.flow, "key-press-event",
                     G_CALLBACK(on_flow_key_press), nullptr);

    // Enter/Space on the selected tile -> route to the tile's own
    // "clicked" handler (launch / drill-down / back).
    g_signal_connect(g_ui.flow, "child-activated",
                     G_CALLBACK(on_flow_child_activated), nullptr);

    // Keep real keyboard focus in sync with the flowbox selection so
    // the tile's :focus CSS lights up when arrowing around.
    g_signal_connect(g_ui.flow, "selected-children-changed",
                     G_CALLBACK(on_flow_selected_changed), nullptr);

    gtk_container_add(GTK_CONTAINER(g_ui.scroll), g_ui.flow);

    gtk_widget_show_all(g_ui.window);
}


// ===========================================================================
// Section 21: UI lock & reattachment
// ===========================================================================

bool claim_ui_lock()
{
    g_ui.lock_fd = ::open(
        UI_LOCK_FILE,
        O_RDWR | O_CREAT | O_CLOEXEC,
        0600);

    if (g_ui.lock_fd < 0) {
        std::fprintf(stderr,
                     "[lattice] cannot open %s: %s\n",
                     UI_LOCK_FILE, std::strerror(errno));
        return false;
    }

    if (::flock(g_ui.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            std::fprintf(stderr,
                         "[lattice] resident UI already locked\n");
        } else {
            std::fprintf(stderr,
                         "[lattice] flock failed: %s\n",
                         std::strerror(errno));
        }
        ::close(g_ui.lock_fd);
        g_ui.lock_fd = -1;
        return false;
    }

    write_pid_file(UI_PID_FILE);
    return true;
}

void release_ui_lock()
{
    if (g_ui.lock_fd >= 0) {
        ::flock(g_ui.lock_fd, LOCK_UN);
        ::close(g_ui.lock_fd);
        g_ui.lock_fd = -1;
    }

    std::error_code ec;
    fs::remove(UI_PID_FILE, ec);
}

bool try_signal_existing_ui()
{
    pid_t pid = 0;
    if (!read_pid_file(UI_PID_FILE, pid)) return false;
    if (pid == ::getpid()) return false;
    if (!pid_is_lattice(pid)) return false;
    if (!pid_is_alive(pid)) return false;
    if (::kill(pid, SIGUSR1) != 0) return false;

    std::fprintf(stderr,
                 "[lattice] signalled resident UI (pid %d)\n", pid);
    return true;
}


// ===========================================================================
// Section 22: GtkApplication activate
// ===========================================================================

void activate(GtkApplication* application, gpointer)
{
    if (g_ui.window != nullptr) {
        show_ui();
        return;
    }

    g_ui.application = application;

    if (!claim_ui_lock()) {
        try_signal_existing_ui();
        g_application_quit(G_APPLICATION(application));
        return;
    }

    build_skeleton();
    show_ui();
}

} // namespace


// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv)
{
    if (!ensure_fresh_lattice_dir())
        return 1;

    spawn_daemon_if_needed();

    if (try_signal_existing_ui())
        return 0;

    g_unix_signal_add(SIGUSR1, on_unix_sigusr1, nullptr);

    GtkApplication* application =
        gtk_application_new(
            "org.lattice.Lattice",
            G_APPLICATION_NON_UNIQUE);

    g_signal_connect(application, "activate",
                     G_CALLBACK(activate), nullptr);

    int status = g_application_run(
        G_APPLICATION(application), argc, argv);

    release_ui_lock();
    g_object_unref(application);

    return status;
}
