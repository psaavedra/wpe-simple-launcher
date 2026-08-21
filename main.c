#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WPE_SIMPLE_LAUNCHER_VERSION_MAJOR 1
#define WPE_SIMPLE_LAUNCHER_VERSION_MINOR 0

// Upper bound for a single unterminated command kept in the input buffer.
#define CTRL_PENDING_MAX (64 * 1024)

// Size of a single read from the control FIFO, and how much may be consumed
// in one dispatch before the main loop is given a turn.
#define CTRL_READ_CHUNK 4096
#define CTRL_DRAIN_MAX (64 * 1024)

// Global variables
static WebKitWebView *web_view;
static gchar *current_uri = NULL;
static int automation = 0;
static int maximized = 0;
static int fullscreen = 0;
static gchar *ctrl_file_path = NULL;
static gchar *feature_list = NULL;

// Control channel state: a FIFO we read commands from, the source watching it,
// and a buffer holding the bytes of a command not yet terminated by a newline.
static int ctrl_fd = -1;
static guint ctrl_watch_id = 0;
static GString *ctrl_pending = NULL;
static gboolean ctrl_discarding = FALSE;

static gboolean automation_views_limit_reached = FALSE;

static WebKitFeature* find_feature(WebKitFeatureList *feature_list, const char *identifier)
{
    for (gsize i = 0; i < webkit_feature_list_get_length(feature_list); i++) {
        WebKitFeature *feature = webkit_feature_list_get(feature_list, i);
        if (!g_ascii_strcasecmp(identifier, webkit_feature_get_identifier(feature)))
            return feature;
    }
    return NULL;
}

static void maximize_window(WebKitWebView *web_view, gboolean maximized) {
    WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
    if (maximized) {
        wpe_toplevel_maximize(toplevel);
    } else {
        wpe_toplevel_unmaximize(toplevel);
    }
}

static void fullscreen_window(WebKitWebView *web_view, gboolean fullscreen) {
    WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
    if (fullscreen) {
        wpe_toplevel_fullscreen(toplevel);
    } else {
        wpe_toplevel_unfullscreen(toplevel);
    }
}

// Execute a single command received through the control FIFO. Anything that is
// not a known command is treated as a URI to load.
static void execute_ctrl_command(const gchar *command) {
    g_autofree gchar *cmd = g_strdup(command);
    g_strstrip(cmd);

    // Empty lines are ignored, and so is 'done' for compatibility with clients
    // written against the old poll-and-acknowledge control file protocol.
    if (cmd[0] == '\0' || !g_strcmp0(cmd, "done"))
        return;

    g_message("Processing action: %s", cmd);

    if (!g_strcmp0(cmd, "back")) {
        webkit_web_view_go_back(web_view);
    } else if (!g_strcmp0(cmd, "forward")) {
        webkit_web_view_go_forward(web_view);
    } else if (!g_strcmp0(cmd, "reload")) {
        webkit_web_view_reload(web_view);
    } else if (!g_strcmp0(cmd, "stop")) {
        webkit_web_view_stop_loading(web_view);
    } else if (!g_strcmp0(cmd, "unfullscreen")) {
        fullscreen_window(web_view, FALSE);
    } else if (!g_strcmp0(cmd, "fullscreen")) {
        fullscreen_window(web_view, TRUE);
    } else if (!g_strcmp0(cmd, "unmaximized")) {
        maximize_window(web_view, FALSE);
    } else if (!g_strcmp0(cmd, "maximized")) {
        maximize_window(web_view, TRUE);
    } else {
        // Assume the command is a URI. Unlike the old polling protocol there is
        // no need to skip a repeated URI: every command is delivered once, so
        // asking for the current URI again is an explicit request to load it.
        const gchar *uri = cmd;

        // The explicit 'load <URI>' form. The command is already stripped, so
        // a bare 'load' ends at the NUL terminator and must be diagnosed here
        // rather than being loaded as the literal URI 'load'. Requiring the
        // NUL or a separator keeps URIs such as 'loader.example' out of this
        // branch.
        if (g_str_has_prefix(cmd, "load")) {
            gchar *argument = cmd + strlen("load");
            if (*argument == '\0' || g_ascii_isspace(*argument)) {
                uri = g_strchug(argument);
                if (uri[0] == '\0') {
                    g_warning("Ignoring 'load' command without a URI.");
                    return;
                }
            }
        }
        webkit_web_view_load_uri(web_view, uri);
        g_free(current_uri);
        current_uri = g_strdup(uri);
    }
}

// Consume every complete (newline terminated) command sitting in the buffer,
// and keep the buffer bounded. Must be called as the buffer grows, not only
// once the FIFO has been drained.
static void process_pending_commands(void) {
    for (;;) {
        const gchar *newline = memchr(ctrl_pending->str, '\n', ctrl_pending->len);

        // The remains of an over-long command are dropped up to and including
        // the next newline, so that its tail is never run as a command.
        if (ctrl_discarding) {
            if (!newline) {
                g_string_truncate(ctrl_pending, 0);
                return;
            }
            g_string_erase(ctrl_pending, 0, (newline - ctrl_pending->str) + 1);
            ctrl_discarding = FALSE;
            continue;
        }

        if (!newline)
            break;

        gsize length = newline - ctrl_pending->str;
        g_autofree gchar *line = g_strndup(ctrl_pending->str, length);
        g_string_erase(ctrl_pending, 0, length + 1);
        execute_ctrl_command(line);
    }

    // A client that never terminates its command must not grow the buffer
    // without bound.
    if (ctrl_pending->len > CTRL_PENDING_MAX) {
        g_warning("Discarding %" G_GSIZE_FORMAT " bytes of unterminated command data.",
                  ctrl_pending->len);
        g_string_truncate(ctrl_pending, 0);
        ctrl_discarding = TRUE;
    }
}

// Called whenever the control FIFO has data available.
static gboolean on_ctrl_readable(gint fd, GIOCondition condition, gpointer user_data) {
    if (condition & (G_IO_ERR | G_IO_NVAL)) {
        g_warning("Control FIFO '%s' is no longer readable, stopping.", ctrl_file_path);
        ctrl_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    gboolean keep_watching = TRUE;
    gsize drained = 0;

    for (;;) {
        gchar buffer[CTRL_READ_CHUNK];
        gssize count = read(fd, buffer, sizeof(buffer));

        if (count > 0) {
            g_string_append_len(ctrl_pending, buffer, count);
            drained += count;

            // Run the commands and apply the buffer limit as the data comes
            // in. Waiting for the FIFO to run dry would let a client that
            // keeps writing grow the buffer without any bound.
            process_pending_commands();

            // Hand the main loop back to its other sources once in a while.
            // The descriptor stays readable, so this callback is dispatched
            // again and the remaining input is picked up from there.
            if (drained >= CTRL_DRAIN_MAX)
                break;

            continue;
        }

        // The FIFO is also held open for writing by this process, so a reader
        // EOF can only mean the descriptor went away.
        if (count == 0) {
            keep_watching = FALSE;
            break;
        }

        int err = errno;

        if (err == EINTR)
            continue;

        // Nothing left to read for now, come back when the poll says so.
        if (err == EAGAIN || err == EWOULDBLOCK)
            break;

        // Give up on an unexpected error: the poll would keep reporting the
        // descriptor as readable and spin this callback forever.
        g_warning("Failed to read from control FIFO '%s': %s", ctrl_file_path, g_strerror(err));
        keep_watching = FALSE;
        break;
    }

    process_pending_commands();

    if (!keep_watching) {
        g_warning("No longer listening for commands on '%s'.", ctrl_file_path);
        ctrl_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

// Create (if needed) and start watching the control FIFO.
static gboolean ctrl_channel_open(GError **error) {
    if (mkfifo(ctrl_file_path, 0600) != 0) {
        int err = errno;

        if (err != EEXIST) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                        "Cannot create control FIFO '%s': %s", ctrl_file_path, g_strerror(err));
            return FALSE;
        }

        // A pre-existing FIFO is reused, which is also how a FIFO left behind
        // by an earlier run is picked up again. This check only produces a
        // helpful message for the common case of a stale regular file at the
        // path; the descriptor opened below is what actually gets validated.
        GStatBuf info;
        if (g_lstat(ctrl_file_path, &info) != 0) {
            err = errno;
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                        "Cannot stat control file '%s': %s", ctrl_file_path, g_strerror(err));
            return FALSE;
        }
        if (!S_ISFIFO(info.st_mode)) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                        "Control file '%s' exists and is not a FIFO (symbolic links are "
                        "not followed). Remove it or choose another path with --ctrl.",
                        ctrl_file_path);
            return FALSE;
        }
    }

    // O_RDWR keeps a writer reference of our own alive: without it the poll
    // would report an endless hangup every time the last client disconnects.
    // O_NOFOLLOW rejects a symbolic link at the end of the path, so the
    // pathname cannot redirect the launcher to some other object.
    ctrl_fd = open(ctrl_file_path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (ctrl_fd < 0) {
        int err = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                    "Cannot open control FIFO '%s': %s", ctrl_file_path, g_strerror(err));
        return FALSE;
    }

    // Everything checked so far was looked up by pathname, and the path may
    // have been replaced since. Validate the descriptor that will actually be
    // read from, so that commands can never be consumed from another object.
    GStatBuf opened;
    if (fstat(ctrl_fd, &opened) != 0) {
        int err = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                    "Cannot stat the opened control FIFO '%s': %s",
                    ctrl_file_path, g_strerror(err));
        close(ctrl_fd);
        ctrl_fd = -1;
        return FALSE;
    }

    if (!S_ISFIFO(opened.st_mode)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                    "'%s' was replaced by another kind of file while it was being "
                    "opened. Refusing to read commands from it.", ctrl_file_path);
        close(ctrl_fd);
        ctrl_fd = -1;
        return FALSE;
    }

    ctrl_pending = g_string_new(NULL);
    ctrl_watch_id = g_unix_fd_add(ctrl_fd, G_IO_IN | G_IO_ERR, on_ctrl_readable, NULL);
    g_message("Listening for commands on control FIFO: %s", ctrl_file_path);

    return TRUE;
}

static void ctrl_channel_close(void) {
    if (ctrl_watch_id) {
        g_source_remove(ctrl_watch_id);
        ctrl_watch_id = 0;
    }

    if (ctrl_fd >= 0) {
        close(ctrl_fd);
        ctrl_fd = -1;
    }

    if (ctrl_pending) {
        g_string_free(ctrl_pending, TRUE);
        ctrl_pending = NULL;
    }

    ctrl_discarding = FALSE;

    // The FIFO itself is left in place. Removing it would mean deciding
    // whether the pathname still refers to the FIFO this process created, and
    // a stale one costs nothing: the next run reuses whatever is already
    // there.
}

static gboolean on_web_view_event(WPEView* view, WPEEvent* event, WebKitWebView* webView)
{
    if (wpe_event_get_event_type(event) != WPE_EVENT_KEYBOARD_KEY_DOWN)
        return FALSE;

    guint keyval = wpe_event_keyboard_get_keyval(event);
    WPEToplevel* toplevel = wpe_view_get_toplevel(view);

    if (keyval == WPE_KEY_F11) {
        if (toplevel) {
            if (wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_FULLSCREEN)
                wpe_toplevel_unfullscreen(toplevel);
            else
                wpe_toplevel_fullscreen(toplevel);
            return TRUE;
        }
    }

    return FALSE;
}

static void on_web_view_close(WebKitWebView *web_view, void *user_data)
{
    g_object_unref(web_view);
}

static gboolean on_signal_quit(GMainLoop *loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

static WebKitWebView *on_automation_create_web_view(WebKitAutomationSession *session, void *user_data)
{
    if (automation_views_limit_reached)
        return NULL;

    automation_views_limit_reached = TRUE;
    return web_view;
}

static void
on_automation_will_close(WebKitAutomationSession *session)
{
    automation_views_limit_reached = FALSE;
}

static void
on_web_context_automation_started(WebKitWebContext *context, WebKitAutomationSession *session, gpointer *view)
{
    g_autoptr(WebKitApplicationInfo) info = webkit_application_info_new();
    webkit_application_info_set_version(info, WPE_SIMPLE_LAUNCHER_VERSION_MAJOR, WPE_SIMPLE_LAUNCHER_VERSION_MINOR, 0);
    webkit_application_info_set_name(info, "wpe-simple-launcher");
    webkit_automation_session_set_application_info(session, info);

    g_signal_connect(session, "create-web-view", G_CALLBACK(on_automation_create_web_view), view);
    g_signal_connect(session, "will-close", G_CALLBACK(on_automation_will_close), NULL);
}

static void print_help(const char *program_name) {
    g_printerr("Usage: %s [OPTIONS] [URL]\n", program_name);
    g_printerr("\nOptions:\n");
    g_printerr("  --automation                  Enable automation mode.\n");
    g_printerr("  --fullscreen                  Start in fullscreen mode.\n");
    g_printerr("  --maximized                   Start in maximized mode.\n");
    g_printerr("  --ctrl <file_path>            Specify control FIFO path (default: wpe-simple-launcher.ctrl).\n");
    g_printerr("  --features <feature_list>     Specify comma-separated list of features to enable.\n");
    g_printerr("  --help                        Show this help message.\n");
    g_printerr("\nControl FIFO:\n");
    g_printerr("  The launcher creates the FIFO at startup unless it already exists, and\n");
    g_printerr("  leaves it in place on exit. Send one newline terminated command per line,\n");
    g_printerr("  for example:\n");
    g_printerr("\n    echo reload > wpe-simple-launcher.ctrl\n\n");
    g_printerr("  Commands: back, forward, reload, stop, fullscreen, unfullscreen, maximized,\n");
    g_printerr("  unmaximized, 'load <URI>', or any other text taken as a URI to load.\n");
}

static void print_features_help(const char *program_name)
{
    g_print("Multiple feature names may be specified separated by commas. No prefix or '+' enable\n"
            "features, prefixes '-' and '!' disable features. Names are case-insensitive. Example:\n"
            "\n    %s --features='!DirPseudo,+WebAnimationsCustomEffects,webgl'\n\n"
            "Available features (+/- = enabled/disabled by default):\n\n", program_name);
    g_autoptr(GEnumClass) statusEnum = (GEnumClass*)(g_type_class_ref(WEBKIT_TYPE_FEATURE_STATUS));
    g_autoptr(WebKitFeatureList) features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature* feature = webkit_feature_list_get(features, i);
        g_print("  %c %s (%s)",
                webkit_feature_get_default_value(feature) ? '+' : '-',
                webkit_feature_get_identifier(feature),
                g_enum_get_value(statusEnum, webkit_feature_get_status(feature))->value_nick);
        if (webkit_feature_get_name(feature))
            g_print(": %s", webkit_feature_get_name(feature));
        g_print("\n");
    }
}

static void cleanup(void) {
    ctrl_channel_close();
    g_free(ctrl_file_path);
    g_free(feature_list);
    g_free(current_uri);
}

int main(int argc, char *argv[]) {
    atexit(cleanup);
    static struct option long_options[] = {
        {"automation", no_argument, &automation, 1},
        {"ctrl", required_argument, 0, 'c'},
        {"fullscreen", no_argument, &fullscreen, 1},
        {"help", no_argument, 0, 'h'},
        {"maximized", no_argument, &maximized, 1},
        {"features", required_argument, 0, 'F'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "c:F:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                ctrl_file_path = g_strdup(optarg);
                break;
            case 'F':
                feature_list = g_strdup(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                return 0;
            case '?':
                return 1;
        }
    }

    if (optind < argc) {
        current_uri = g_strdup(argv[optind]);
    }

    if (!ctrl_file_path) {
        ctrl_file_path = g_strdup("wpe-simple-launcher.ctrl");
    }

    if (!g_strcmp0(feature_list, "help")) {
        print_features_help(argv[0]);
        return 0;
    }

    WebKitWebContext *web_context = webkit_web_context_get_default();
    webkit_web_context_set_automation_allowed(web_context, (automation == 1));

    // Create a new WebKitWebView
    g_autoptr(WebKitSettings) settings = webkit_settings_new();

    if (feature_list) {
        g_autoptr(WebKitFeatureList) features = webkit_settings_get_all_features();
        g_auto(GStrv) items = g_strsplit(feature_list, ",", -1);
        for (gsize i = 0; items[i]; i++) {
            char* item = g_strstrip(items[i]);
            gboolean enabled = TRUE;
            switch (item[0]) {
            case '!':
            case '-':
                enabled = FALSE;
            case '+':
                item++;
            default:
                break;
            }

            if (item[0] == '\0') {
                g_printerr("Empty feature name specified, skipped.\n");
                continue;
            }

            WebKitFeature* feature = find_feature(features, item);
            if (feature)
                webkit_settings_set_feature_enabled(settings, feature, enabled);
            else
                g_printerr("Feature '%s' is not available.\n", item);
        }
    }

    g_autoptr(WebKitWebsitePolicies) website_policy = webkit_website_policies_new();
    web_view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
                            "settings", settings,
                            "web-context", web_context,
                            "website-policies", website_policy,
                            "is-controlled-by-automation", (automation == 1),
                            NULL);

    // webkit_web_view_get_wpe_view() returns a borrowed reference
    WPEView *wpe_view = webkit_web_view_get_wpe_view(web_view);
    if (wpe_view) {
        g_signal_connect(wpe_view, "event", G_CALLBACK(on_web_view_event), NULL);
    }
    g_signal_connect(web_view, "close", G_CALLBACK(on_web_view_close), NULL);

    if (automation) {
        g_signal_connect(web_context,
                         "automation-started",
                         G_CALLBACK(on_web_context_automation_started), web_view);
    }

    WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
    wpe_toplevel_resize(toplevel, 1024, 768);

    if (maximized) {
        maximize_window(web_view, TRUE);
    }

    if (fullscreen) {
        fullscreen_window(web_view, TRUE);
    }

    if (current_uri) {
        webkit_web_view_load_uri(web_view, current_uri);
    }

    // Create the main loop and catch the termination signals before the
    // control FIFO exists. Once these sources are installed a signal is
    // handled by the loop instead of taking its default action, which would
    // kill the process without running the cleanup and leave the FIFO behind.
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, TRUE);
    g_unix_signal_add(SIGINT, G_SOURCE_FUNC(on_signal_quit), loop);
    g_unix_signal_add(SIGTERM, G_SOURCE_FUNC(on_signal_quit), loop);

    // Listen for commands on the control FIFO
    g_autoptr(GError) ctrl_error = NULL;
    if (!ctrl_channel_open(&ctrl_error)) {
        g_printerr("%s\n", ctrl_error->message);
        return EXIT_FAILURE;
    }

    g_main_loop_run(loop);

    ctrl_channel_close();
    webkit_web_view_try_close(web_view);

    return EXIT_SUCCESS;
}
