#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

// Global variables
static WebKitWebView *web_view;
static gchar *current_uri = NULL;

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

// Timeout function to handle file content
static gboolean load_view(gpointer user_data) {
    const gchar *file_path = (const gchar *)user_data;
    gchar *content = read_file_content(file_path);

    if (content == NULL) {
        g_warning("Failed to read the file: %s", file_path);
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
        WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
        wpe_toplevel_unfullscreen(toplevel);
    } else if (trimmed_strcmp0(content, "fullscreen") == 0) {
        WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
        wpe_toplevel_fullscreen(toplevel);
    } else if (trimmed_strcmp0(content, "unmaximized") == 0) {
        WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
        wpe_toplevel_unmaximize(toplevel);
    } else if (trimmed_strcmp0(content, "maximized") == 0) {
        WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
        wpe_toplevel_maximize(toplevel);
    } else {
        // Assume the content is a URL
        if (current_uri == NULL || trimmed_strcmp0(current_uri, content) != 0) {
            webkit_web_view_load_uri(web_view, content);
            g_free(current_uri);
            current_uri = g_strdup(content);
        }
    }

    // Write 'done' to the file
    write_done_to_file(file_path);
    g_free(content);
    return TRUE;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        g_printerr("Usage: %s <file_path>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const gchar *file_path = argv[1];

    // Create a new WebKitWebView
    g_autoptr(WebKitSettings) settings = webkit_settings_new();
    g_autoptr(WebKitWebsitePolicies) website_policy = webkit_website_policies_new_with_policies(
            "autoplay", WEBKIT_AUTOPLAY_ALLOW,
            NULL);;
    web_view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "settings", settings,
            "web-context", webkit_web_context_get_default(),
            "website-policies", website_policy,
            NULL);

    WPEToplevel *toplevel = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(web_view));
    wpe_toplevel_resize(toplevel, 1024, 768);

    // Set up a timeout to check the file every second
    g_timeout_add(1000, load_view, (gpointer)file_path);

    // Create and run the main loop
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, TRUE);
    g_main_loop_run(loop);

    webkit_web_view_try_close(web_view);

    g_free(current_uri);
    return EXIT_SUCCESS;
}
