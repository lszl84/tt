// tt in the Xfce panel: the Omarchy widget's timer icon, with the running
// task's time next to it. A click shows tt's window, hides it if it is the
// window in use, or starts tt if it isn't running.
//
// Like the Omarchy widget, the plugin is a view: everything it shows comes
// from `tt status`, and it never touches state.json itself. It only watches
// the file, to ask again when the window or a command changes it.

#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <gtk/gtk.h>
#include <libwnck/libwnck.h>
#include <libxfce4panel/libxfce4panel.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#ifndef TT_COMMAND
#define TT_COMMAND "tt"
#endif

namespace {

// The Omarchy widget's glyphs: md-timer (U+F13AB) while idle, dimmed like
// the idle icon in its dropdown, and md-timer_outline (U+F051B) while a
// task runs.
constexpr const char* kIdleIcon = "tt-idle-symbolic";
constexpr const char* kRunningIcon = "tt-running-symbolic";
constexpr double kIdleOpacity = 0.6;

struct TaskTime {
    std::string name;
    long long seconds = 0;  // today, as of the status read
};

struct TtPanel {
    XfcePanelPlugin* plugin = nullptr;  // null when run standalone
    GtkWidget* button = nullptr;
    GtkWidget* image = nullptr;
    GtkWidget* label = nullptr;
    WnckHandle* wnck = nullptr;
    WnckScreen* screen = nullptr;  // null without X11
    std::string command;

    // The last `tt status`.
    bool loaded = false;
    std::string error;
    long long readAt = 0;
    long long todayEnd = 0;
    long long activeSince = 0;
    int active = -1;
    std::vector<TaskTime> today;

    // `tt status` runs one at a time; asking while it runs asks once more
    // when it is done.
    GCancellable* cancellable = nullptr;
    bool querying = false;
    bool queryAgain = false;

    std::string watchedPath;
    GFileMonitor* monitor = nullptr;
    guint debounceId = 0;
    guint tickId = 0;
    guint refreshId = 0;

    GPid launched = 0;  // tt started by a click, until it exits
    guint launchWatchId = 0;
};

void Query(TtPanel* tp);

std::string Pad(long long n) {
    return (n < 10 ? "0" : "") + std::to_string(n);
}

// Same output as FormatDuration() in data.cpp: "1h 02m 03s", "2m 03s", "12s".
std::string FormatDuration(long long seconds) {
    long long total = std::max(0LL, seconds);
    long long h = total / 3600, m = total % 3600 / 60, s = total % 60;
    if (h > 0) return std::to_string(h) + "h " + Pad(m) + "m " + Pad(s) + "s";
    if (m > 0) return std::to_string(m) + "m " + Pad(s) + "s";
    return std::to_string(s) + "s";
}

// "1h 02m" / "12m".
std::string FormatShort(long long seconds) {
    long long total = std::max(0LL, seconds);
    long long h = total / 3600, m = total % 3600 / 60;
    return h > 0 ? std::to_string(h) + "h " + Pad(m) + "m" : std::to_string(m) + "m";
}

// "1:02", the label in the Omarchy bar.
std::string FormatClock(long long seconds) {
    long long total = std::max(0LL, seconds);
    return std::to_string(total / 3600) + ":" + Pad(total % 3600 / 60);
}

std::string Escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string out = escaped;
    g_free(escaped);
    return out;
}

// Wall-clock time, like tt itself, so suspend doesn't skew it.
long long Now() {
    return (long long)std::time(nullptr);
}

bool Running(const TtPanel* tp) {
    return tp->active >= 0 && tp->active < (int)tp->today.size();
}

bool Vertical(const TtPanel* tp) {
    return tp->plugin != nullptr &&
           xfce_panel_plugin_get_mode(tp->plugin) == XFCE_PANEL_PLUGIN_MODE_VERTICAL;
}

// Seconds tracked on task `i` today, counting the running session up to
// `now`.
long long TodaySeconds(const TtPanel* tp, int i, long long now) {
    long long seconds = tp->today[i].seconds;
    if (i == tp->active) {
        seconds += std::max(0LL, std::min(now, tp->todayEnd) - tp->readAt);
    }
    return seconds;
}

// The running task, today's total and today's tasks, longest first.
std::string TooltipMarkup(const TtPanel* tp, long long now) {
    if (!tp->loaded) return Escape(tp->error.empty() ? "Loading" : tp->error);

    std::string markup =
        Running(tp) ? "<b>" + Escape(tp->today[tp->active].name) + "</b> · " +
                          FormatDuration(now - tp->activeSince)
                    : std::string("Not tracking");

    std::vector<std::pair<long long, int>> rows;
    long long total = 0;
    for (int i = 0; i < (int)tp->today.size(); ++i) {
        long long seconds = TodaySeconds(tp, i, now);
        total += seconds;
        if (seconds > 0) rows.push_back({seconds, i});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    markup += "\n" + FormatShort(total) + " today";
    for (const auto& [seconds, i] : rows) {
        markup += "\n    " + Escape(tp->today[i].name) + "  " + FormatShort(seconds);
    }
    if (!tp->error.empty()) markup += "\n" + Escape(tp->error);
    return markup;
}

gboolean Tick(gpointer data);

void UpdateView(TtPanel* tp) {
    const long long now = Now();
    const bool running = Running(tp);

    const char* icon = running ? kRunningIcon : kIdleIcon;
    const gchar* shown = nullptr;
    gtk_image_get_icon_name(GTK_IMAGE(tp->image), &shown, nullptr);
    if (g_strcmp0(shown, icon) != 0) {
        gtk_image_set_from_icon_name(GTK_IMAGE(tp->image), icon, GTK_ICON_SIZE_BUTTON);
    }
    gtk_widget_set_opacity(tp->image, running ? 1.0 : kIdleOpacity);

    // Like the Omarchy bar: the time only while a task runs, and not in a
    // vertical panel.
    gtk_widget_set_visible(tp->label, running && !Vertical(tp));
    if (running) {
        std::string clock = FormatClock(now - tp->activeSince);
        if (clock != gtk_label_get_text(GTK_LABEL(tp->label))) {
            gtk_label_set_text(GTK_LABEL(tp->label), clock.c_str());
        }
    }

    std::string markup = TooltipMarkup(tp, now);
    gchar* current = gtk_widget_get_tooltip_markup(tp->button);
    if (g_strcmp0(current, markup.c_str()) != 0) {
        gtk_widget_set_tooltip_markup(tp->button, markup.c_str());
    }
    g_free(current);

    if (running && tp->tickId == 0) {
        tp->tickId = g_timeout_add_seconds(1, Tick, tp);
    } else if (!running && tp->tickId != 0) {
        g_source_remove(tp->tickId);
        tp->tickId = 0;
    }
}

gboolean Tick(gpointer data) {
    UpdateView(static_cast<TtPanel*>(data));
    return G_SOURCE_CONTINUE;
}

// ---- tt status -------------------------------------------------------------

gboolean Debounced(gpointer data) {
    auto* tp = static_cast<TtPanel*>(data);
    tp->debounceId = 0;
    Query(tp);
    return G_SOURCE_REMOVE;
}

void FileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer data) {
    auto* tp = static_cast<TtPanel*>(data);
    if (tp->debounceId != 0) g_source_remove(tp->debounceId);
    tp->debounceId = g_timeout_add(250, Debounced, tp);
}

void StopWatching(TtPanel* tp) {
    if (tp->monitor == nullptr) return;
    g_signal_handlers_disconnect_by_data(tp->monitor, tp);
    g_file_monitor_cancel(tp->monitor);
    g_object_unref(tp->monitor);
    tp->monitor = nullptr;
}

// Picks up changes from anywhere: the window, tt in a terminal, or a sync.
void Watch(TtPanel* tp, const std::string& path) {
    if (path == tp->watchedPath) return;
    StopWatching(tp);
    tp->watchedPath = path;
    if (path.empty()) return;
    GFile* file = g_file_new_for_path(path.c_str());
    tp->monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(file);
    if (tp->monitor != nullptr) {
        g_signal_connect(tp->monitor, "changed", G_CALLBACK(FileChanged), tp);
    }
}

void ApplyStatus(TtPanel* tp, const std::string& text) {
    try {
        auto reply = nlohmann::json::parse(text);
        if (!reply.value("ok", false)) {
            tp->error = reply.value("error", std::string("tt failed"));
        } else {
            const auto& status = reply.at("status");
            const auto& tasks = status.at("tasks");
            const auto& range = status.at("ranges").at(0);  // today
            const auto& seconds = range.at("seconds");

            std::vector<TaskTime> today;
            int active = -1;
            for (size_t i = 0; i < tasks.size(); ++i) {
                today.push_back({tasks[i].value("name", std::string()),
                                 i < seconds.size() ? seconds[i].get<long long>() : 0});
                if (tasks[i].value("active", false)) active = (int)i;
            }
            tp->readAt = status.at("now").get<long long>();
            tp->todayEnd = range.at("end").get<long long>();
            tp->activeSince = status.value("activeSince", 0LL);
            tp->today = std::move(today);
            tp->active = active;
            tp->loaded = true;
            tp->error.clear();
            Watch(tp, status.value("dataPath", std::string()));
        }
    } catch (const std::exception&) {
        tp->error = text.empty() ? "tt did not answer" : "Unexpected output from tt";
    }
    UpdateView(tp);
}

void StatusDone(GObject* source, GAsyncResult* result, gpointer data) {
    gchar* out = nullptr;
    GError* error = nullptr;
    bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out,
                                                   nullptr, &error);
    if (!ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);  // the plugin is gone
        return;
    }

    auto* tp = static_cast<TtPanel*>(data);
    tp->querying = false;
    if (ok) {
        ApplyStatus(tp, out != nullptr ? out : "");
    } else {
        tp->error = error->message;
        UpdateView(tp);
    }
    g_free(out);
    if (error != nullptr) g_error_free(error);

    if (tp->queryAgain) {
        tp->queryAgain = false;
        Query(tp);
    }
}

void Query(TtPanel* tp) {
    if (tp->querying) {
        tp->queryAgain = true;
        return;
    }
    GError* error = nullptr;
    GSubprocess* process = g_subprocess_new(
        GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
        &error, tp->command.c_str(), "status", nullptr);
    if (process == nullptr) {
        tp->error = std::string("Could not run tt: ") + error->message;
        g_error_free(error);
        UpdateView(tp);
        return;
    }
    tp->querying = true;
    g_subprocess_communicate_utf8_async(process, nullptr, tp->cancellable, StatusDone, tp);
    g_object_unref(process);
}

gboolean Refresh(gpointer data) {
    Query(static_cast<TtPanel*>(data));
    return G_SOURCE_CONTINUE;
}

// ---- The window ------------------------------------------------------------

void ShowError(const char* title, const char* detail) {
    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GtkDialogFlags(0), GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    gtk_widget_show(dialog);
}

// tt's window, the topmost if there are several.
WnckWindow* FindWindow(TtPanel* tp) {
    if (tp->screen == nullptr) return nullptr;
    WnckWindow* found = nullptr;
    for (GList* l = wnck_screen_get_windows_stacked(tp->screen); l != nullptr; l = l->next) {
        auto* window = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(window) == WNCK_WINDOW_NORMAL &&
            g_strcmp0(wnck_window_get_class_instance_name(window), "tt") == 0) {
            found = window;
        }
    }
    return found;
}

void LaunchExited(GPid pid, gint status, gpointer data) {
    auto* tp = static_cast<TtPanel*>(data);
    g_spawn_close_pid(pid);
    tp->launched = 0;
    tp->launchWatchId = 0;
}

void Launch(TtPanel* tp) {
    if (tp->launched != 0) return;  // still starting up
    // --window: the window even where tt would live in the Omarchy bar.
    const gchar* argv[] = {tp->command.c_str(), "--window", nullptr};
    GError* error = nullptr;
    if (!g_spawn_async(g_get_home_dir(), const_cast<gchar**>(argv), nullptr,
                       G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &tp->launched, &error)) {
        ShowError("Could not start tt", error->message);
        g_error_free(error);
        tp->launched = 0;
        return;
    }
    tp->launchWatchId = g_child_watch_add(tp->launched, LaunchExited, tp);
}

// The way the tasklist toggles a window: hide it if it is the one in use,
// otherwise bring it up, from another workspace or minimized too.
void Clicked(GtkButton*, gpointer data) {
    auto* tp = static_cast<TtPanel*>(data);
    WnckWindow* window = FindWindow(tp);
    if (window == nullptr) {
        Launch(tp);
        return;
    }
    WnckWorkspace* workspace = wnck_screen_get_active_workspace(tp->screen);
    bool inView = workspace != nullptr ? wnck_window_is_visible_on_workspace(window, workspace)
                                       : !wnck_window_is_minimized(window);
    // Most recently activated rather than active: clicking the panel can
    // take the focus away from it.
    if (inView && wnck_window_is_most_recently_activated(window)) {
        wnck_window_minimize(window);
    } else {
        wnck_window_activate_transient(window, gtk_get_current_event_time());
    }
}

// ---- Setup -----------------------------------------------------------------

// The tt this plugin was built with, else the first one in PATH.
std::string TtCommand() {
    if (g_path_is_absolute(TT_COMMAND) && g_file_test(TT_COMMAND, G_FILE_TEST_IS_EXECUTABLE)) {
        return TT_COMMAND;
    }
    gchar* found = g_find_program_in_path("tt");
    std::string command = found != nullptr ? found : TT_COMMAND;
    g_free(found);
    return command;
}

TtPanel* TtPanelNew(XfcePanelPlugin* plugin) {
    auto* tp = new TtPanel;
    tp->plugin = plugin;
    tp->command = TtCommand();
    tp->cancellable = g_cancellable_new();

#ifdef TT_PANEL_ICON_DIR
    // tt's icons, in a directory of their own outside the icon themes.
    gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), TT_PANEL_ICON_DIR);
#endif

    tp->button = xfce_panel_create_button();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    tp->image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(tp->image), 16);
    tp->label = gtk_label_new(nullptr);
    gtk_widget_set_no_show_all(tp->label, TRUE);
    gtk_box_pack_start(GTK_BOX(box), tp->image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tp->label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tp->button), box);
    g_signal_connect(tp->button, "clicked", G_CALLBACK(Clicked), tp);

    // The pager client type: window managers honour a pager's requests to
    // activate a window, where they might refuse an application's.
    tp->wnck = wnck_handle_new(WNCK_CLIENT_TYPE_PAGER);
    tp->screen = wnck_handle_get_default_screen(tp->wnck);
    if (tp->screen != nullptr) wnck_screen_force_update(tp->screen);

    UpdateView(tp);
    Query(tp);
    // Today ends at midnight without any change to the file.
    tp->refreshId = g_timeout_add_seconds(60, Refresh, tp);
    gtk_widget_show_all(tp->button);
    return tp;
}

void TtPanelFree(TtPanel* tp) {
    g_cancellable_cancel(tp->cancellable);
    g_object_unref(tp->cancellable);
    StopWatching(tp);
    for (guint id : {tp->debounceId, tp->tickId, tp->refreshId, tp->launchWatchId}) {
        if (id != 0) g_source_remove(id);
    }
    g_object_unref(tp->wnck);
    delete tp;
}

gboolean SizeChanged(XfcePanelPlugin* plugin, gint size, gpointer data) {
    auto* tp = static_cast<TtPanel*>(data);
    size /= xfce_panel_plugin_get_nrows(plugin);
    // Square with only the icon; the time widens it.
    gtk_widget_set_size_request(tp->button, size, size);
    gtk_image_set_pixel_size(GTK_IMAGE(tp->image), xfce_panel_plugin_get_icon_size(plugin));
    return TRUE;
}

void ModeChanged(XfcePanelPlugin*, XfcePanelPluginMode, gpointer data) {
    UpdateView(static_cast<TtPanel*>(data));
}

void FreeData(XfcePanelPlugin*, gpointer data) {
    TtPanelFree(static_cast<TtPanel*>(data));
}

}  // namespace

// Called from register.c.
extern "C" void tt_panel_construct(XfcePanelPlugin* plugin) {
    TtPanel* tp = TtPanelNew(plugin);
    gtk_container_add(GTK_CONTAINER(plugin), tp->button);
    xfce_panel_plugin_add_action_widget(plugin, tp->button);
    xfce_panel_plugin_set_small(plugin, TRUE);

    g_signal_connect(plugin, "size-changed", G_CALLBACK(SizeChanged), tp);
    g_signal_connect(plugin, "mode-changed", G_CALLBACK(ModeChanged), tp);
    g_signal_connect(plugin, "free-data", G_CALLBACK(FreeData), tp);
    SizeChanged(plugin, xfce_panel_plugin_get_size(plugin), tp);
}

#ifdef TT_PANEL_STANDALONE

// The button in a window of its own, to try it without the panel:
//   cmake --build build --target tt-panel-window && build/tt-panel-window
int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "tt panel button");
    gtk_window_set_default_size(GTK_WINDOW(window), 160, 32);
    // Like the panel, keep the focus where it is, or a click could never
    // find tt's window in use and hide it.
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
    TtPanel* tp = TtPanelNew(nullptr);
    gtk_container_add(GTK_CONTAINER(window), tp->button);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(window);
    gtk_main();
    TtPanelFree(tp);
    return 0;
}

#endif
