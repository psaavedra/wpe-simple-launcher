#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <getopt.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#define WPE_SIMPLE_LAUNCHER_VERSION_MAJOR 1
#define WPE_SIMPLE_LAUNCHER_VERSION_MINOR 0

// Global variables
static WebKitWebView *web_view;
static gchar *current_uri = NULL;
static int automation = 0;
static int maximized = 0;

static gboolean automation_views_limit_reached = FALSE;

// Function to compare two strings after trimming whitespace
int trimmed_strcmp0(const char *str1, const char *str2)
{
    // Duplicate the strings to avoid modifying the original ones
    char *trimmed_str1 = str1 ? g_strdup(str1) : NULL;
    char *trimmed_str2 = str2 ? g_strdup(str2) : NULL;

    // Trim the strings
    if (trimmed_str1) g_strstrip(trimmed_str1);
    if (trimmed_str2) g_strstrip(trimmed_str2);

    // Compare the trimmed strings
    int result = g_strcmp0(trimmed_str1, trimmed_str2);

    // Free the duplicated strings
    g_free(trimmed_str1);
    g_free(trimmed_str2);

    return result;
}

// Function to read the content of the file
static gchar* read_file_content(const gchar *file_path) {
    gchar *content = NULL;
    g_file_get_contents(file_path, &content, NULL, NULL);
    return content;
}

// Function to write 'done' to the file
static void write_done_to_file(const gchar *file_path) {
    g_file_set_contents(file_path, "done", -1, NULL);
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

// Timeout function to handle file content
static gboolean load_view(gpointer user_data) {
    const gchar *ctrl_file_path = (const gchar *)user_data;
    gchar *content = read_file_content(ctrl_file_path);

    if (content == NULL) {
        g_warning("Failed to read the file: %s", ctrl_file_path);
        return TRUE;
    }

    if (trimmed_strcmp0(content, "done") == 0) {
        return TRUE;
    }

    g_message("Processing action: %s", content);

    if (trimmed_strcmp0(content, "back") == 0) {
        webkit_web_view_go_back(web_view);
    } else if (trimmed_strcmp0(content, "forward") == 0) {
        webkit_web_view_go_forward(web_view);
    } else if (trimmed_strcmp0(content, "reload") == 0) {
        webkit_web_view_reload(web_view);
    } else if (trimmed_strcmp0(content, "unfullscreen") == 0) {
        fullscreen_window(web_view, FALSE);
    } else if (trimmed_strcmp0(content, "fullscreen") == 0) {
        fullscreen_window(web_view, TRUE);
    } else if (trimmed_strcmp0(content, "unmaximized") == 0) {
        maximize_window(web_view, FALSE);
    } else if (trimmed_strcmp0(content, "maximized") == 0) {
        maximize_window(web_view, TRUE);
    } else {
        // Assume the content is a URL
        if (current_uri == NULL || trimmed_strcmp0(current_uri, content) != 0) {
            webkit_web_view_load_uri(web_view, content);
            g_free(current_uri);
            current_uri = g_strdup(content);
        }
    }

    // Write 'done' to the file
    write_done_to_file(ctrl_file_path);
    g_free(content);
    return TRUE;
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

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"maximized", no_argument, &maximized, 1},
        {"automation", no_argument, &automation, 1},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
        if (c == '?') {
            // Unknown option
            return 1;
        }
    }

    if (optind >= argc) {
        g_printerr("Usage: %s [--maximized] [--automation] <ctrl_file_path>\n", argv[0]);
        return 1;
    }

    const gchar *ctrl_file_path = argv[optind];

    WebKitWebContext *web_context = webkit_web_context_get_default();
    webkit_web_context_set_automation_allowed(web_context, (automation == 1));

    // Create a new WebKitWebView
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    g_autoptr(WebKitWebsitePolicies) website_policy = webkit_website_policies_new();
    web_view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
                            "settings", settings,
                            "web-context", web_context,
                            "website-policies", website_policy,
                            "is-controlled-by-automation", (automation == 1),
                            NULL);
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

    // Set up a timeout to check the file every second
    g_timeout_add(1000, load_view, (gpointer)ctrl_file_path);

    // Create and run the main loop
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, TRUE);
    g_main_loop_run(loop);
    g_unix_signal_add(SIGINT, G_SOURCE_FUNC(on_signal_quit), loop);
    g_unix_signal_add(SIGTERM, G_SOURCE_FUNC(on_signal_quit), loop);

    webkit_web_view_try_close(web_view);

    g_free(current_uri);
    return EXIT_SUCCESS;
}
